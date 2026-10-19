/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * IR 0.3 (numeric profile fixed_q32_v1) scalar x86-64 lowering through the pinned AsmJit. Same kernel ABI as the
 * M1 scalar lowering (su_native_asmjit.cpp): one SuNativeArgs pointer (rdi on SysV, rcx on Win64), rows in
 * ascending order, every writable lane stored for every row, returns 0.
 *
 * Plan (no optimizer):
 *  - Register r lives in a 32-byte slot at [rsp + 32 + 32r] with the VM's register-file layout: u32 and bool
 *    zero-extended in word 0, i32 sign-extended in word 0, i64 and q32 in word 0, v3 in words 0..2, quat (w, x,
 *    y, z) in words 0..3. The verifier guarantees types and definition before use; no runtime tags.
 *  - [rsp, rsp + 32) is the shadow space of the Win64 convention for helper calls (unused on SysV). Column base
 *    pointers follow the register slots. The frame is a multiple of 16 and the three pushes keep rsp 16-byte
 *    aligned at every call.
 *  - rbx = args, r12 = row index, r13 = row count: callee-saved in both conventions, so helper calls preserve
 *    them. Temporaries: rax, rcx, rdx, r8, r9 (volatile in both).
 *  - Inline: constants, loads and stores (64-bit fields as two u32 lanes, low word first), u32/bool ops, i32 and
 *    i64 wrapping arithmetic, compares, min/max/clamp, selects, saturating q32 add/sub/neg/abs, the q32 multiply
 *    (signed 128-bit product, round half to even, saturation), conversions without rounding, v3 add/sub/neg/scale,
 *    make/get/select of v3 and quat.
 *  - Calls: division, shifts, sqrt/rsqrt, q32_from_i64, q32_to_i32_round, dot, cross, len, len_sq, normalize,
 *    clamp_len and the quaternion operations call C helpers (su_native_ops03.c) with pointers to the dst and
 *    operand slots in the first four argument registers.
 *  - A frame over one page is probed page by page from the top; zero rows return before anything is saved.
 *  - Identity-row guard: when the VM plan builder proved one (src/vm/su_vm_plan.h), each row first runs the guard
 *    slice; rows that meet its requirements copy their writable lanes from the input lanes and skip the body.
 *
 * The C side refuses modules with entity operations; every opcode reaching this file is lowered or the lowering
 * fails with SU_NLOW_INTERNAL (no partial kernels). */
#include <asmjit/x86.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "backends/native/su_native_lower.h"
#include "backends/native/su_native_unwind.h"

using namespace asmjit;
using namespace asmjit::x86;

namespace {

class RecordingErrorHandler : public ErrorHandler {
public:
    Error first = kErrorOk;
    char text[160] = {0};

    void handle_error(Error err, const char *message, BaseEmitter *origin) override {
        (void)origin;
        if (first == kErrorOk) {
            first = err;
            if (message != nullptr) {
                std::snprintf(text, sizeof text, "%s", message);
            }
        }
    }
};

#if defined(_WIN32)
constexpr Gp kArgReg = rcx;
const Gp kCallArg[4] = {rcx, rdx, r8, r9};
#else
constexpr Gp kArgReg = rdi;
const Gp kCallArg[4] = {rdi, rsi, rdx, rcx};
#endif

constexpr int32_t kShadow = 32;
constexpr int32_t kSlot = 32;
constexpr int32_t kPage = 4096;
constexpr uint64_t kFrameLimit = UINT64_C(1) << 20;

struct Ins {
    uint32_t op, dst, a, b, c;
};

/* Register allocation (2026-10-18). A scalar IR register (u32, bool, i32, i64, q32: one 64-bit word) may live in a
 * general-purpose register instead of its slot for the whole kernel:
 *  - callee-saved pool (rbp, r14, r15; rsi, rdi on Win64): survives helper calls and the row loop, so it can hold
 *    row-invariant values (hoisted constants) and values whose live range spans a helper call;
 *  - volatile pool (r10, r11; rsi, rdi on SysV): only for live ranges with no helper call strictly inside them.
 * Neither pool is touched by the inline templates (rax, rcx, rdx, r8, r9) or holds rbx/r12/r13 (args, row, rows).
 * Live range of a register = [first, last] body position that accesses it; a hoisted (row-invariant) register is
 * live for the whole loop. Registers are assigned greedily by access count, each to a pool register whose
 * assigned ranges do not overlap its own. Around a helper call, allocated operands are stored to their slots
 * first and an allocated destination is reloaded from its slot after. Vector registers (v3, quat) stay in slots.
 * Values are bit-for-bit those of the slot layout (u32/bool zero-extended, i32 sign-extended), so allocation does
 * not change any result; ir03_diff.py --native checks that end to end. */
struct Roles {
    uint32_t reads[3];
    bool read_vec[3];
    uint32_t nreads;
    bool has_def;
    bool def_vec;
};

bool is_vec_def(uint32_t op) {
    switch (op) {
    case SU_NLOW_OP_LOAD_V3:
    case SU_NLOW_OP_MAKE_V3:
    case SU_NLOW_OP_V3_ADD:
    case SU_NLOW_OP_V3_SUB:
    case SU_NLOW_OP_V3_SCALE:
    case SU_NLOW_OP_V3_NEG:
    case SU_NLOW_OP_V3_CROSS:
    case SU_NLOW_OP_V3_NORMALIZE:
    case SU_NLOW_OP_V3_CLAMP_LEN:
    case SU_NLOW_OP_SELECT_V3:
    case SU_NLOW_OP_MAKE_QUAT:
    case SU_NLOW_OP_SELECT_QUAT:
    case SU_NLOW_OP_QUAT_MUL:
    case SU_NLOW_OP_QUAT_CONJ:
    case SU_NLOW_OP_QUAT_ROTATE:
    case SU_NLOW_OP_QUAT_NORMALIZE:
    case SU_NLOW_OP_QUAT_INTEGRATE:
        return true;
    default:
        return false;
    }
}

/* Register operands of one instruction. False for an opcode this table does not know (allocation is then off). */
bool roles_of(const Ins &in, Roles &r) {
    const uint32_t op = in.op;
    auto rd = [&](uint32_t reg, bool vec) {
        r.reads[r.nreads] = reg;
        r.read_vec[r.nreads] = vec;
        r.nreads++;
    };
    r.nreads = 0;
    r.has_def = true;
    r.def_vec = is_vec_def(op);
    if (op >= SU_NLOW_OP_I32_BASE && op < SU_NLOW_OP_Q32_BASE + SU_NLOW_X_COUNT) {
        const uint32_t x = (op - SU_NLOW_OP_I32_BASE) % 32u;
        if (x >= SU_NLOW_X_COUNT) {
            return false;
        }
        rd(in.a, false);
        if (x != SU_NLOW_X_NEG && x != SU_NLOW_X_ABS) {
            rd(in.b, false);
        }
        if (x == SU_NLOW_X_CLAMP || x == SU_NLOW_X_SELECT) {
            rd(in.c, false);
        }
        return true;
    }
    switch (op) {
    case SU_NLOW_OP_CONST_U32:
    case SU_NLOW_OP_CONST_I32:
    case SU_NLOW_OP_CONST_I64:
    case SU_NLOW_OP_CONST_Q32:
    case SU_NLOW_OP_LOAD_U32:
    case SU_NLOW_OP_LOAD_I32:
    case SU_NLOW_OP_LOAD_I64:
    case SU_NLOW_OP_LOAD_Q32:
    case SU_NLOW_OP_LOAD_V3:
        return true;
    case SU_NLOW_OP_STORE_U32:
    case SU_NLOW_OP_STORE_I32:
    case SU_NLOW_OP_STORE_I64:
    case SU_NLOW_OP_STORE_Q32:
        r.has_def = false;
        rd(in.b, false);
        return true;
    case SU_NLOW_OP_STORE_V3:
        r.has_def = false;
        rd(in.dst, true);
        return true;
    case SU_NLOW_OP_ADD_U32:
    case SU_NLOW_OP_SUB_U32:
    case SU_NLOW_OP_MUL_U32:
    case SU_NLOW_OP_EQ_U32:
    case SU_NLOW_OP_LT_U32:
    case SU_NLOW_OP_AND_BOOL:
    case SU_NLOW_OP_OR_BOOL:
    case SU_NLOW_OP_AND_U32:
    case SU_NLOW_OP_OR_U32:
    case SU_NLOW_OP_XOR_U32:
    case SU_NLOW_OP_SHL_U32:
    case SU_NLOW_OP_SHR_U32:
    case SU_NLOW_OP_DIV_U32:
    case SU_NLOW_OP_REM_U32:
    case SU_NLOW_OP_AND_I64:
    case SU_NLOW_OP_OR_I64:
    case SU_NLOW_OP_XOR_I64:
        rd(in.a, false);
        rd(in.b, false);
        return true;
    case SU_NLOW_OP_SELECT_U32:
        rd(in.a, false);
        rd(in.b, false);
        rd(in.c, false);
        return true;
    case SU_NLOW_OP_NOT_BOOL:
    case SU_NLOW_OP_NOT_U32:
    case SU_NLOW_OP_NOT_I64:
    case SU_NLOW_OP_SQRT_Q32:
    case SU_NLOW_OP_RSQRT_Q32:
    case SU_NLOW_OP_I32_FROM_U32:
    case SU_NLOW_OP_U32_FROM_I32:
    case SU_NLOW_OP_I64_FROM_I32:
    case SU_NLOW_OP_I32_FROM_I64:
    case SU_NLOW_OP_Q32_FROM_I32:
    case SU_NLOW_OP_Q32_FROM_I64:
    case SU_NLOW_OP_Q32_TO_I32_FLOOR:
    case SU_NLOW_OP_Q32_TO_I32_ROUND:
    case SU_NLOW_OP_Q32_FROM_RAW:
    case SU_NLOW_OP_Q32_RAW:
    case SU_NLOW_OP_Q32_TO_I64_FLOOR:
        rd(in.a, false);
        return true;
    case SU_NLOW_OP_MAKE_V3:
        rd(in.a, false);
        rd(in.b, false);
        rd(in.c, false);
        return true;
    case SU_NLOW_OP_V3_GET:
    case SU_NLOW_OP_QUAT_GET:
    case SU_NLOW_OP_V3_NEG:
    case SU_NLOW_OP_V3_LEN:
    case SU_NLOW_OP_V3_LEN_SQ:
    case SU_NLOW_OP_V3_NORMALIZE:
    case SU_NLOW_OP_QUAT_CONJ:
    case SU_NLOW_OP_QUAT_NORMALIZE:
        rd(in.a, true);
        return true;
    case SU_NLOW_OP_V3_ADD:
    case SU_NLOW_OP_V3_SUB:
    case SU_NLOW_OP_V3_DOT:
    case SU_NLOW_OP_V3_CROSS:
    case SU_NLOW_OP_QUAT_MUL:
    case SU_NLOW_OP_QUAT_ROTATE:
        rd(in.a, true);
        rd(in.b, true);
        return true;
    case SU_NLOW_OP_V3_SCALE:
    case SU_NLOW_OP_V3_CLAMP_LEN:
        rd(in.a, true);
        rd(in.b, false);
        return true;
    case SU_NLOW_OP_SELECT_V3:
    case SU_NLOW_OP_SELECT_QUAT:
        rd(in.a, false);
        rd(in.b, true);
        rd(in.c, true);
        return true;
    case SU_NLOW_OP_MAKE_QUAT:
        rd(in.a, false);
        rd(in.b, true);
        return true;
    case SU_NLOW_OP_QUAT_INTEGRATE:
        rd(in.a, true);
        rd(in.b, true);
        rd(in.c, false);
        return true;
    default:
        return false;
    }
}

#if defined(_WIN32)
const Gp kSavedPool[] = {r14, r15, rbp, rsi, rdi};
const Gp kVolatilePool[] = {r10, r11};
#else
const Gp kSavedPool[] = {r14, r15, rbp};
const Gp kVolatilePool[] = {r10, r11, rsi, rdi};
#endif
constexpr uint32_t kNSaved = sizeof kSavedPool / sizeof kSavedPool[0];
constexpr uint32_t kNVolatile = sizeof kVolatilePool / sizeof kVolatilePool[0];

/* Result of the allocation: phys[r] = index into the combined pool (saved first, then volatile), or -1. */
struct Allocation {
    std::vector<int32_t> phys;
    uint32_t saved_used = 0;  /* bit k: kSavedPool[k] is used (pushed in the prologue) */
    uint32_t allocated = 0;
};

Gp pool_reg(int32_t p) {
    return p < static_cast<int32_t>(kNSaved) ? kSavedPool[p] : kVolatilePool[p - static_cast<int32_t>(kNSaved)];
}

/* Computes the allocation for the body (instructions in index order; hoisted instructions run before the loop and
 * keep their registers for every row). helper[k] says whether instruction k calls a C helper. */
Allocation allocate(const std::vector<Ins> &insns, uint32_t nregs, const std::vector<bool> &hoisted,
                    const std::vector<bool> &helper, bool enabled) {
    Allocation out;
    out.phys.assign(nregs, -1);
    if (!enabled || nregs == 0u) {
        return out;
    }
    const int64_t kWhole = INT64_MAX / 4;
    std::vector<int64_t> first(nregs, INT64_MAX), last(nregs, -1);
    std::vector<uint32_t> weight(nregs, 0u);
    std::vector<bool> vec(nregs, false), whole(nregs, false), known(nregs, false);
    std::vector<int64_t> calls;
    const int64_t n = static_cast<int64_t>(insns.size());
    for (int64_t k = 0; k < n; k++) {
        Roles ro;
        if (!roles_of(insns[static_cast<size_t>(k)], ro)) {
            return out; /* unknown opcode: no allocation at all */
        }
        auto touch = [&](uint32_t reg, bool v) {
            if (reg >= nregs) {
                return false;
            }
            first[reg] = std::min(first[reg], k);
            last[reg] = std::max(last[reg], k);
            weight[reg]++;
            known[reg] = true;
            if (v) {
                vec[reg] = true;
            }
            if (hoisted[static_cast<size_t>(k)]) {
                whole[reg] = true;
            }
            return true;
        };
        for (uint32_t i = 0; i < ro.nreads; i++) {
            if (!touch(ro.reads[i], ro.read_vec[i])) {
                return out;
            }
        }
        if (ro.has_def && !touch(insns[static_cast<size_t>(k)].dst, ro.def_vec)) {
            return out;
        }
        if (helper[static_cast<size_t>(k)]) {
            calls.push_back(k);
        }
    }
    std::vector<uint32_t> order;
    for (uint32_t r = 0; r < nregs; r++) {
        if (known[r] && !vec[r]) {
            if (whole[r]) {
                first[r] = -1;
                last[r] = kWhole;
            }
            order.push_back(r);
        }
    }
    /* Most accesses first; ties by register number (deterministic). */
    std::stable_sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) { return weight[x] > weight[y]; });
    std::vector<std::vector<std::pair<int64_t, int64_t>>> busy(kNSaved + kNVolatile);
    for (uint32_t r : order) {
        if (weight[r] < 2u) {
            continue; /* one access: a register saves nothing */
        }
        bool spans_call = whole[r];
        for (int64_t c : calls) {
            if (c > first[r] && c < last[r]) {
                spans_call = true;
                break;
            }
        }
        /* Volatile registers first for call-free ranges (keeps the callee-saved ones, which cost a push, free). */
        int32_t pick = -1;
        for (uint32_t pass = 0; pass < 2u && pick < 0; pass++) {
            const bool vol = pass == 0u;
            if (vol && spans_call) {
                continue;
            }
            const uint32_t lo = vol ? kNSaved : 0u, hi = vol ? kNSaved + kNVolatile : kNSaved;
            for (uint32_t p = lo; p < hi && pick < 0; p++) {
                bool free_here = true;
                for (const auto &iv : busy[p]) {
                    if (first[r] <= iv.second && iv.first <= last[r]) {
                        free_here = false;
                        break;
                    }
                }
                if (free_here) {
                    pick = static_cast<int32_t>(p);
                }
            }
        }
        if (pick >= 0) {
            busy[static_cast<size_t>(pick)].push_back({first[r], last[r]});
            out.phys[r] = pick;
            out.allocated++;
            if (pick < static_cast<int32_t>(kNSaved)) {
                out.saved_used |= 1u << static_cast<uint32_t>(pick);
            }
        }
    }
    return out;
}

class Lowerer {
public:
    Lowerer(Assembler &as, const std::vector<uint32_t> &rord, const std::vector<uint32_t> &word, uint32_t nin,
            int32_t col_base, const std::vector<int32_t> &phys)
        : a(as), rord_(rord), word_(word), nin_(nin), col_base_(col_base), phys_(phys) {}

    /* Word k of register r: its allocated GPR (k == 0 only) or its slot. */
    Operand S(uint32_t r, uint32_t k = 0) const {
        if (k == 0u && r < phys_.size() && phys_[r] >= 0) {
            return pool_reg(phys_[r]);
        }
        return M(r, k);
    }
    /* Low 32 bits of register r. */
    Operand SD(uint32_t r) const {
        if (r < phys_.size() && phys_[r] >= 0) {
            return pool_reg(phys_[r]).r32();
        }
        return dword_ptr(rsp, kShadow + static_cast<int32_t>(r) * kSlot);
    }
    /* The slot itself (vector words, helper arguments, spills). */
    Mem M(uint32_t r, uint32_t k = 0) const {
        return qword_ptr(rsp, kShadow + static_cast<int32_t>(r) * kSlot + 8 * static_cast<int32_t>(k));
    }
    bool in_reg(uint32_t r) const { return r < phys_.size() && phys_[r] >= 0; }
    int32_t slot_off(uint32_t r) const { return kShadow + static_cast<int32_t>(r) * kSlot; }
    void E(InstId id, const Operand_ &x, const Operand_ &y) { a.emit(id, x, y); }

    void in_col(const Gp &dst, uint32_t field) { a.mov(dst, qword_ptr(rsp, col_base_ + 8 * static_cast<int32_t>(rord_[field]))); }
    void out_col(const Gp &dst, uint32_t field) {
        a.mov(dst, qword_ptr(rsp, col_base_ + 8 * static_cast<int32_t>(nin_ + word_[field])));
    }

    /* rax = 64-bit value of the two-lane field (lanes field, field + 1) at the current row. */
    void load64(uint32_t field) {
        in_col(rcx, field);
        a.mov(eax, dword_ptr(rcx, r12, 2));
        in_col(rcx, field + 1u);
        a.mov(edx, dword_ptr(rcx, r12, 2));
        a.shl(rdx, 32);
        a.or_(rax, rdx);
    }
    /* Stores rax into the two lanes of field. Clobbers rax, rcx. */
    void store64(uint32_t field) {
        out_col(rcx, field);
        a.mov(dword_ptr(rcx, r12, 2), eax);
        a.shr(rax, 32);
        out_col(rcx, field + 1u);
        a.mov(dword_ptr(rcx, r12, 2), eax);
    }

    /* rax = sat(rax + rcx) */
    void q_add_rax_rcx() {
        a.mov(rdx, rcx);
        a.sar(rdx, 63);
        a.mov(r8, INT64_MAX);
        a.xor_(r8, rdx); /* b >= 0: MAX, b < 0: MIN */
        a.add(rax, rcx);
        a.cmovo(rax, r8);
    }
    /* rax = sat(rax - rcx) */
    void q_sub_rax_rcx() {
        a.mov(rdx, rcx);
        a.sar(rdx, 63);
        a.mov(r8, INT64_MIN);
        a.xor_(r8, rdx); /* b >= 0: MIN, b < 0: MAX */
        a.sub(rax, rcx);
        a.cmovo(rax, r8);
    }
    /* rax = sat(-rax) */
    void q_neg_rax() {
        a.neg(rax);
        a.mov(rcx, INT64_MAX);
        a.cmovo(rax, rcx);
    }
    /* rax = RNE(rax * rcx / 2^32), saturating: signed 128-bit product, floor shift by 32, round half to even on
     * the discarded 32 bits (round half to even is symmetric, so this equals the reference's sign-magnitude
     * rounding), then saturation when the quotient does not fit 64 bits. */
    void q_mul_rax_rcx() {
        /* Branchless: the rounding fraction is data-dependent and close to random, so branches would mispredict. */
        a.imul(rdx, rax, rcx);       /* rdx:rax = signed product */
        a.mov(r9d, eax);             /* frac = low 32 bits */
        a.shrd(rax, rdx, 32);        /* rax = bits 32..95 */
        a.sar(rdx, 32);              /* rdx = bits 96..127, sign-extended: q = rdx:rax */
        a.xor_(ecx, ecx);
        a.cmp(r9d, 0x80000000u);
        a.seta(cl);                  /* frac > 1/2 */
        a.sete(r8b);                 /* frac == 1/2 ... */
        a.and_(r8b, al);             /* ... and q odd (r8b is 0 or 1, so only bit 0 of al matters) */
        a.or_(cl, r8b);
        a.add(rax, rcx);             /* round up */
        a.adc(rdx, 0);
        /* Saturate when q does not fit 64 bits (rdx is not the sign extension of rax). */
        a.mov(rcx, rax);
        a.sar(rcx, 63);
        a.xor_(rcx, rdx);            /* 0 when q fits */
        a.mov(r8, rdx);
        a.sar(r8, 63);
        a.mov(r9, INT64_MAX);
        a.xor_(r9, r8);              /* q >= 0: MAX, q < 0: MIN */
        a.test(rcx, rcx);
        a.cmovne(rax, r9);
    }

    /* Allocated operands go to their slots first (the helper reads slots); an allocated destination is reloaded. */
    void call_helper(uint64_t fn, uint32_t d, uint32_t ra, uint32_t rb, uint32_t rc) {
        const uint32_t ops[3] = {ra, rb, rc};
        for (uint32_t i = 0; i < 3u; i++) {
            if (in_reg(ops[i])) {
                a.mov(M(ops[i]), pool_reg(phys_[ops[i]]));
            }
        }
        a.lea(kCallArg[0], ptr(rsp, slot_off(d)));
        a.lea(kCallArg[1], ptr(rsp, slot_off(ra)));
        a.lea(kCallArg[2], ptr(rsp, slot_off(rb)));
        a.lea(kCallArg[3], ptr(rsp, slot_off(rc)));
        a.mov(rax, fn);
        a.call(rax);
        if (in_reg(d)) {
            a.mov(pool_reg(phys_[d]), M(d));
        }
    }

    Assembler &a;

private:
    const std::vector<uint32_t> &rord_;
    const std::vector<uint32_t> &word_;
    uint32_t nin_;
    int32_t col_base_;
    const std::vector<int32_t> &phys_;
};

bool is_family(uint32_t op, uint32_t base) { return op >= base && op < base + SU_NLOW_X_COUNT; }

/* Register operands of a helper op: 1 (a), 2 (a, b) or 3 (a, b, c). Operand fields that are not registers may
 * hold SU_OPERAND_NONE, so only these become slot addresses; the others repeat a. */
uint32_t helper_arity(uint32_t op) {
    switch (op) {
    case SU_NLOW_OP_SQRT_Q32:
    case SU_NLOW_OP_RSQRT_Q32:
    case SU_NLOW_OP_Q32_FROM_I64:
    case SU_NLOW_OP_Q32_TO_I32_ROUND:
    case SU_NLOW_OP_V3_LEN:
    case SU_NLOW_OP_V3_LEN_SQ:
    case SU_NLOW_OP_V3_NORMALIZE:
    case SU_NLOW_OP_QUAT_CONJ:
    case SU_NLOW_OP_QUAT_NORMALIZE:
        return 1u;
    case SU_NLOW_OP_QUAT_INTEGRATE:
        return 3u;
    default:
        return 2u; /* div, shl, shr, dot, cross, clamp_len, quat_mul, quat_rotate */
    }
}

} // namespace

extern "C" uint32_t su_native_lower_x64_03(const SuNativeLowerInput *in, SuNativeLowered *out, char *msg,
                                           size_t msg_cap, uint32_t *failed_insn) {
    if (in == nullptr || out == nullptr || in->helpers == nullptr) {
        return SU_NLOW_INTERNAL;
    }
    std::memset(out, 0, sizeof *out);
    if (failed_insn != nullptr) {
        *failed_insn = UINT32_MAX;
    }
    try {
        const uint32_t nregs = in->register_count;
        const uint32_t nfields = in->field_count;
        const uint32_t ninsn = in->insn_count;
        std::vector<uint32_t> rord(nfields, 0u), word(nfields, 0u);
        uint32_t nin = 0, nout = 0;
        for (uint32_t i = 0; i < nfields; i++) {
            if ((in->field_access[i] & SU_NLOW_ACCESS_READ) != 0u) {
                rord[i] = nin++;
            }
            if ((in->field_access[i] & SU_NLOW_ACCESS_WRITE) != 0u) {
                word[i] = nout++;
            }
        }
        const uint64_t col_base = static_cast<uint64_t>(kShadow) + static_cast<uint64_t>(kSlot) * (nregs > 0u ? nregs : 1u);
        uint64_t frame = (col_base + 8u * static_cast<uint64_t>(nin + nout) + 15u) & ~static_cast<uint64_t>(15u);
        if (frame > kFrameLimit) {
            if (msg != nullptr && msg_cap > 0u) {
                std::snprintf(msg, msg_cap, "frame of %llu bytes exceeds the IR 0.3 lowering bound",
                              static_cast<unsigned long long>(frame));
            }
            return SU_NLOW_RESOURCE;
        }

        std::vector<Ins> insns(ninsn);
        for (uint32_t k = 0; k < ninsn; k++) {
            const uint32_t *w = in->insn_words + 5u * static_cast<size_t>(k);
            insns[k] = Ins{w[0], w[1], w[2], w[3], w[4]};
        }
        /* Hoisted (row-invariant) instructions run once before the loop and are not repeated per row: their registers
         * are defined once and nothing else writes them (the plan builder's condition), so the value persists. */
        std::vector<bool> hoisted(ninsn, false), calls(ninsn, false);
        for (uint32_t i = 0; i < in->pre_count; i++) {
            if (in->pre_insns[i] < ninsn) {
                hoisted[in->pre_insns[i]] = true;
            }
        }
        for (uint32_t k = 0; k < ninsn; k++) {
            calls[k] = insns[k].op < SU_NLOW_HELPER_SLOTS && in->helpers[insns[k].op] != 0u;
        }
        /* SUTEKH_NATIVE_REGALLOC=0 lowers with every register in its slot (diagnostic and differential switch). */
        const char *ra_env = std::getenv("SUTEKH_NATIVE_REGALLOC");
        const Allocation alloc = allocate(insns, nregs, hoisted, calls, !(ra_env != nullptr && std::strcmp(ra_env, "0") == 0));
        /* Pushes: rbx, r12, r13 and every callee-saved pool register in use. rsp is 16-byte aligned at each call when
         * the pushes plus the return address plus the frame are a multiple of 16. */
        uint32_t pushes = 3u;
        for (uint32_t p = 0; p < kNSaved; p++) {
            pushes += (alloc.saved_used >> p) & 1u;
        }
        if (pushes % 2u == 0u) {
            frame += 8u;
        }

        Environment env(Arch::kX64);
        CodeHolder code;
        if (code.init(env) != kErrorOk) {
            return SU_NLOW_INTERNAL;
        }
        RecordingErrorHandler eh;
        code.set_error_handler(&eh);
        StringLogger logger;
        Assembler a(&code);
        a.set_logger(&logger);
        Lowerer L(a, rord, word, nin, static_cast<int32_t>(col_base), alloc.phys);

        std::vector<uint32_t> ranges;
        ranges.reserve(2u * static_cast<size_t>(ninsn));
        Label l_zero = a.new_label();
        Label l_loop = a.new_label();

        a.cmp(qword_ptr(kArgReg, 8), 0);
        a.je(l_zero);
        su_unwind::Builder uw;
        a.push(rbx);
        uw.push(a.offset(), rbx.id());
        a.push(r12);
        uw.push(a.offset(), r12.id());
        a.push(r13);
        uw.push(a.offset(), r13.id());
        for (uint32_t p = 0; p < kNSaved; p++) {
            if ((alloc.saved_used >> p) & 1u) {
                a.push(kSavedPool[p]);
                uw.push(a.offset(), kSavedPool[p].id());
            }
        }
        a.mov(rbx, kArgReg);
        a.sub(rsp, static_cast<int32_t>(frame));
        uw.alloc(a.offset(), static_cast<uint32_t>(frame));
        uw.end_prolog(a.offset());
        if (frame > static_cast<uint64_t>(kPage)) {
            for (int64_t off = static_cast<int64_t>(frame) - 8; off >= 0; off -= kPage) {
                a.mov(qword_ptr(rsp, static_cast<int32_t>(off)), 0);
            }
        }
        a.mov(rax, qword_ptr(rbx, 16));
        for (uint32_t i = 0; i < nin; i++) {
            a.mov(rcx, qword_ptr(rax, static_cast<int32_t>(8u * i)));
            a.mov(qword_ptr(rsp, static_cast<int32_t>(col_base + 8u * i)), rcx);
        }
        a.mov(rax, qword_ptr(rbx, 24));
        for (uint32_t j = 0; j < nout; j++) {
            a.mov(rcx, qword_ptr(rax, static_cast<int32_t>(8u * j)));
            a.mov(qword_ptr(rsp, static_cast<int32_t>(col_base + 8u * (nin + j))), rcx);
        }
        a.xor_(r12d, r12d);
        a.mov(r13, qword_ptr(rbx, 8));

        /* Emits one instruction; false when the opcode is not lowered. Scalar operands go through L.S / L.SD, which
         * name an allocated GPR or the slot; vector words always use L.M. */
        auto emit = [&](const Ins &ins) -> bool {
            const uint32_t op = ins.op, d = ins.dst, ra = ins.a, rb = ins.b, rc = ins.c;
            const uint64_t helper = op < SU_NLOW_HELPER_SLOTS ? in->helpers[op] : 0u;
            bool ok = true;

            if (helper != 0u) {
                const uint32_t n = helper_arity(op);
                L.call_helper(helper, d, ra, n >= 2u ? rb : ra, n >= 3u ? rc : ra);
            } else if (is_family(op, SU_NLOW_OP_I32_BASE) || is_family(op, SU_NLOW_OP_I64_BASE) ||
                       is_family(op, SU_NLOW_OP_Q32_BASE)) {
                const uint32_t base = op >= SU_NLOW_OP_Q32_BASE ? SU_NLOW_OP_Q32_BASE
                                      : (op >= SU_NLOW_OP_I64_BASE ? SU_NLOW_OP_I64_BASE : SU_NLOW_OP_I32_BASE);
                const uint32_t x = op - base;
                const bool i32 = base == SU_NLOW_OP_I32_BASE, q32 = base == SU_NLOW_OP_Q32_BASE;
                switch (x) {
                case SU_NLOW_X_ADD:
                case SU_NLOW_X_SUB:
                case SU_NLOW_X_MUL:
                    if (i32) {
                        L.E(Inst::kIdMov, eax, L.SD(ra));
                        L.E(x == SU_NLOW_X_ADD ? Inst::kIdAdd : (x == SU_NLOW_X_SUB ? Inst::kIdSub : Inst::kIdImul), eax,
                            L.SD(rb));
                        a.movsxd(rax, eax);
                    } else if (!q32) {
                        L.E(Inst::kIdMov, rax, L.S(ra));
                        L.E(x == SU_NLOW_X_ADD ? Inst::kIdAdd : (x == SU_NLOW_X_SUB ? Inst::kIdSub : Inst::kIdImul), rax,
                            L.S(rb));
                    } else {
                        L.E(Inst::kIdMov, rax, L.S(ra));
                        L.E(Inst::kIdMov, rcx, L.S(rb));
                        if (x == SU_NLOW_X_ADD) L.q_add_rax_rcx();
                        else if (x == SU_NLOW_X_SUB) L.q_sub_rax_rcx();
                        else L.q_mul_rax_rcx();
                    }
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_X_NEG:
                    if (i32) {
                        L.E(Inst::kIdMov, eax, L.SD(ra));
                        a.neg(eax);
                        a.movsxd(rax, eax);
                    } else {
                        L.E(Inst::kIdMov, rax, L.S(ra));
                        if (q32) L.q_neg_rax();
                        else a.neg(rax);
                    }
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_X_ABS:
                    if (i32) {
                        L.E(Inst::kIdMov, eax, L.SD(ra));
                        a.mov(ecx, eax);
                        a.neg(ecx);
                        a.test(eax, eax);
                        a.cmovs(eax, ecx);
                        a.movsxd(rax, eax);
                    } else {
                        L.E(Inst::kIdMov, rax, L.S(ra));
                        a.mov(rcx, rax);
                        a.neg(rcx);
                        if (q32) {
                            a.mov(rdx, INT64_MAX);
                            a.cmovo(rcx, rdx);
                        }
                        a.test(rax, rax);
                        a.cmovs(rax, rcx);
                    }
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_X_MIN:
                case SU_NLOW_X_MAX:
                    L.E(Inst::kIdMov, rax, L.S(ra));
                    L.E(Inst::kIdMov, rcx, L.S(rb));
                    a.cmp(rcx, rax);
                    if (x == SU_NLOW_X_MIN) a.cmovl(rax, rcx);
                    else a.cmovg(rax, rcx);
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_X_CLAMP:
                    L.E(Inst::kIdMov, rax, L.S(ra));
                    L.E(Inst::kIdMov, rcx, L.S(rb));
                    a.cmp(rcx, rax);
                    a.cmovg(rax, rcx);
                    L.E(Inst::kIdMov, rcx, L.S(rc));
                    a.cmp(rcx, rax);
                    a.cmovl(rax, rcx);
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_X_EQ:
                case SU_NLOW_X_LT:
                case SU_NLOW_X_LE:
                    L.E(Inst::kIdMov, rax, L.S(ra));
                    L.E(Inst::kIdCmp, rax, L.S(rb));
                    if (x == SU_NLOW_X_EQ) a.sete(al);
                    else if (x == SU_NLOW_X_LT) a.setl(al);
                    else a.setle(al);
                    a.movzx(eax, al);
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_X_SELECT:
                    L.E(Inst::kIdMov, rax, L.S(rc));
                    L.E(Inst::kIdCmp, L.S(ra), Imm(0));
                    L.E(Inst::kIdCmovne, rax, L.S(rb));
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                default:
                    ok = false; /* div, shl, shr come with helpers */
                    break;
                }
            } else {
                switch (op) {
                case SU_NLOW_OP_CONST_U32:
                    a.mov(eax, ra);
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_CONST_I32:
                    a.mov(rax, static_cast<int64_t>(static_cast<int32_t>(ra)));
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_CONST_I64:
                case SU_NLOW_OP_CONST_Q32:
                    a.mov(rax, (static_cast<uint64_t>(rb) << 32) | static_cast<uint64_t>(ra));
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_LOAD_U32:
                    L.in_col(rcx, ra);
                    a.mov(eax, dword_ptr(rcx, r12, 2));
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_LOAD_I32:
                    L.in_col(rcx, ra);
                    a.movsxd(rax, dword_ptr(rcx, r12, 2));
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_LOAD_I64:
                case SU_NLOW_OP_LOAD_Q32:
                    L.load64(ra);
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_LOAD_V3:
                    L.load64(ra);
                    a.mov(L.M(d, 0), rax);
                    L.load64(rb);
                    a.mov(L.M(d, 1), rax);
                    L.load64(rc);
                    a.mov(L.M(d, 2), rax);
                    break;
                case SU_NLOW_OP_STORE_U32:
                case SU_NLOW_OP_STORE_I32:
                    L.E(Inst::kIdMov, eax, L.SD(rb));
                    L.out_col(rcx, ra);
                    a.mov(dword_ptr(rcx, r12, 2), eax);
                    break;
                case SU_NLOW_OP_STORE_I64:
                case SU_NLOW_OP_STORE_Q32:
                    L.E(Inst::kIdMov, rax, L.S(rb));
                    L.store64(ra);
                    break;
                case SU_NLOW_OP_STORE_V3:
                    a.mov(rax, L.M(d, 0));
                    L.store64(ra);
                    a.mov(rax, L.M(d, 1));
                    L.store64(rb);
                    a.mov(rax, L.M(d, 2));
                    L.store64(rc);
                    break;
                case SU_NLOW_OP_ADD_U32:
                case SU_NLOW_OP_SUB_U32:
                case SU_NLOW_OP_MUL_U32:
                    L.E(Inst::kIdMov, eax, L.SD(ra));
                    L.E(op == SU_NLOW_OP_ADD_U32 ? Inst::kIdAdd : (op == SU_NLOW_OP_SUB_U32 ? Inst::kIdSub : Inst::kIdImul),
                        eax, L.SD(rb));
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_EQ_U32:
                case SU_NLOW_OP_LT_U32:
                    L.E(Inst::kIdMov, eax, L.SD(ra));
                    L.E(Inst::kIdCmp, eax, L.SD(rb));
                    if (op == SU_NLOW_OP_EQ_U32) a.sete(al);
                    else a.setb(al);
                    a.movzx(eax, al);
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_SELECT_U32:
                    L.E(Inst::kIdMov, rax, L.S(rc));
                    L.E(Inst::kIdCmp, L.S(ra), Imm(0));
                    L.E(Inst::kIdCmovne, rax, L.S(rb));
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_AND_BOOL:
                case SU_NLOW_OP_OR_BOOL:
                    L.E(Inst::kIdMov, rax, L.S(ra));
                    L.E(op == SU_NLOW_OP_AND_BOOL ? Inst::kIdAnd : Inst::kIdOr, rax, L.S(rb));
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_NOT_BOOL:
                    L.E(Inst::kIdMov, rax, L.S(ra));
                    a.xor_(rax, 1);
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                /* IR 0.4 bit operations: u32 results zero-extended (a 32-bit mov clears bits 32..63). */
                case SU_NLOW_OP_AND_U32:
                case SU_NLOW_OP_OR_U32:
                case SU_NLOW_OP_XOR_U32:
                    L.E(Inst::kIdMov, eax, L.SD(ra));
                    L.E(op == SU_NLOW_OP_AND_U32 ? Inst::kIdAnd : (op == SU_NLOW_OP_OR_U32 ? Inst::kIdOr : Inst::kIdXor),
                        eax, L.SD(rb));
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_NOT_U32:
                    L.E(Inst::kIdMov, eax, L.SD(ra));
                    a.not_(eax);
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_SHL_U32:
                case SU_NLOW_OP_SHR_U32:
                    /* n >= 32 gives 0; the hardware masks the count to 5 bits, so select 0 explicitly. */
                    L.E(Inst::kIdMov, eax, L.SD(ra));
                    L.E(Inst::kIdMov, ecx, L.SD(rb));
                    if (op == SU_NLOW_OP_SHL_U32) a.shl(eax, cl);
                    else a.shr(eax, cl);
                    a.xor_(edx, edx);
                    a.cmp(ecx, 32);
                    a.cmovae(eax, edx);
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_DIV_U32:
                case SU_NLOW_OP_REM_U32: {
                    /* b = 0: div gives 0, rem gives a. */
                    Label l_zero_div = a.new_label(), l_done_div = a.new_label();
                    L.E(Inst::kIdMov, eax, L.SD(ra));
                    L.E(Inst::kIdMov, ecx, L.SD(rb));
                    a.test(ecx, ecx);
                    a.jz(l_zero_div);
                    a.xor_(edx, edx);
                    a.div(edx, eax, ecx);
                    if (op == SU_NLOW_OP_REM_U32) a.mov(eax, edx);
                    a.jmp(l_done_div);
                    a.bind(l_zero_div);
                    if (op == SU_NLOW_OP_DIV_U32) a.xor_(eax, eax);
                    a.bind(l_done_div);
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                }
                case SU_NLOW_OP_AND_I64:
                case SU_NLOW_OP_OR_I64:
                case SU_NLOW_OP_XOR_I64:
                    L.E(Inst::kIdMov, rax, L.S(ra));
                    L.E(op == SU_NLOW_OP_AND_I64 ? Inst::kIdAnd : (op == SU_NLOW_OP_OR_I64 ? Inst::kIdOr : Inst::kIdXor),
                        rax, L.S(rb));
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_NOT_I64:
                    L.E(Inst::kIdMov, rax, L.S(ra));
                    a.not_(rax);
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_I32_FROM_U32:
                case SU_NLOW_OP_I32_FROM_I64:
                    L.E(Inst::kIdMovsxd, rax, L.SD(ra));
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_U32_FROM_I32:
                    L.E(Inst::kIdMov, eax, L.SD(ra));
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_I64_FROM_I32:
                case SU_NLOW_OP_Q32_FROM_RAW:
                case SU_NLOW_OP_Q32_RAW:
                    L.E(Inst::kIdMov, rax, L.S(ra));
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_Q32_FROM_I32:
                    L.E(Inst::kIdMov, rax, L.S(ra));
                    a.shl(rax, 32);
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_Q32_TO_I32_FLOOR:
                case SU_NLOW_OP_Q32_TO_I64_FLOOR:
                    L.E(Inst::kIdMov, rax, L.S(ra));
                    a.sar(rax, 32);
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_MAKE_V3:
                    L.E(Inst::kIdMov, rax, L.S(ra));
                    a.mov(L.M(d, 0), rax);
                    L.E(Inst::kIdMov, rax, L.S(rb));
                    a.mov(L.M(d, 1), rax);
                    L.E(Inst::kIdMov, rax, L.S(rc));
                    a.mov(L.M(d, 2), rax);
                    break;
                case SU_NLOW_OP_V3_GET:
                case SU_NLOW_OP_QUAT_GET:
                    a.mov(rax, L.M(ra, rb));
                    L.E(Inst::kIdMov, L.S(d), rax);
                    break;
                case SU_NLOW_OP_V3_ADD:
                case SU_NLOW_OP_V3_SUB:
                    for (uint32_t c3 = 0; c3 < 3u; c3++) {
                        a.mov(rax, L.M(ra, c3));
                        a.mov(rcx, L.M(rb, c3));
                        if (op == SU_NLOW_OP_V3_ADD) L.q_add_rax_rcx();
                        else L.q_sub_rax_rcx();
                        a.mov(L.M(d, c3), rax);
                    }
                    break;
                case SU_NLOW_OP_V3_SCALE:
                    for (uint32_t c3 = 0; c3 < 3u; c3++) {
                        a.mov(rax, L.M(ra, c3));
                        L.E(Inst::kIdMov, rcx, L.S(rb));
                        L.q_mul_rax_rcx();
                        a.mov(L.M(d, c3), rax);
                    }
                    break;
                case SU_NLOW_OP_V3_NEG:
                    for (uint32_t c3 = 0; c3 < 3u; c3++) {
                        a.mov(rax, L.M(ra, c3));
                        L.q_neg_rax();
                        a.mov(L.M(d, c3), rax);
                    }
                    break;
                case SU_NLOW_OP_SELECT_V3:
                case SU_NLOW_OP_SELECT_QUAT: {
                    const uint32_t n = op == SU_NLOW_OP_SELECT_V3 ? 3u : 4u;
                    L.E(Inst::kIdCmp, L.S(ra), Imm(0));
                    for (uint32_t c3 = 0; c3 < n; c3++) {
                        a.mov(rax, L.M(rc, c3));
                        a.cmovne(rax, L.M(rb, c3)); /* mov does not change flags */
                        a.mov(L.M(d, c3), rax);
                    }
                    break;
                }
                case SU_NLOW_OP_MAKE_QUAT:
                    L.E(Inst::kIdMov, rax, L.S(ra));
                    a.mov(L.M(d, 0), rax);
                    for (uint32_t c3 = 0; c3 < 3u; c3++) {
                        a.mov(rax, L.M(rb, c3));
                        a.mov(L.M(d, c3 + 1u), rax);
                    }
                    break;
                default:
                    ok = false;
                    break;
                }
            }
            return ok;
        };
        /* Identity-row guard (src/vm/su_vm_plan.h, proven by the VM plan builder): run the guard slice; when every
         * checked definition has its required value, the row's writable lanes equal its input lanes, so they are
         * copied and the body is skipped. Otherwise the body runs (re-running the slice: pure instructions). */
        Label l_body = a.new_label();
        Label l_next = a.new_label();
        std::vector<std::pair<uint32_t, uint32_t>> pre_range(ninsn, {0u, 0u});
        /* Identity lanes: copy each whole input column into its output column once per call (memcpy of rows u32
         * cells). A guarded row then needs no per-lane copy, and every other row's body overwrites every writable
         * lane, so the outputs are exactly the per-row copies' (2026-10-18). */
        /* IR 0.4 row filter: a filtered-out row keeps its input values, so its writable lanes are copied the same way
         * (the identity lanes, when present, are every writable lane and cover them). */
        const uint32_t ncopy = in->identity_count > 0u ? in->identity_count : (in->has_filter ? in->filter_count : 0u);
        const uint32_t *copy_lanes = in->identity_count > 0u ? in->identity_lanes : in->filter_lanes;
        if (ncopy > 0u) {
            a.comment("identity / filter lanes: whole-column copies");
            for (uint32_t i = 0; i < ncopy; i++) {
                const uint32_t lane = copy_lanes[i];
                L.out_col(kCallArg[0], lane);
                L.in_col(kCallArg[1], lane);
                a.mov(kCallArg[2], r13);
                a.shl(kCallArg[2], 2);
                a.mov(rax, reinterpret_cast<uint64_t>(&std::memcpy));
                a.call(rax);
            }
        }
        if (in->pre_count > 0u) {
            a.comment("row-invariant instructions");
            for (uint32_t i = 0; i < in->pre_count; i++) {
                const uint32_t k = in->pre_insns[i];
                const size_t begin = a.offset();
                if (k >= ninsn || !emit(insns[k])) {
                    if (failed_insn != nullptr) {
                        *failed_insn = k;
                    }
                    return SU_NLOW_INTERNAL;
                }
                pre_range[k] = {static_cast<uint32_t>(begin), static_cast<uint32_t>(a.offset())};
            }
        }
        a.bind(l_loop);
        if (in->has_filter) {
            a.comment("row filter");
            L.in_col(rcx, in->filter_field);
            a.mov(eax, dword_ptr(rcx, r12, 2));
            a.and_(eax, in->filter_mask);
            a.cmp(eax, in->filter_value);
            a.jne(l_next); /* the row's writable lanes already hold their input values (copied above) */
        }
        if (in->guard_count > 0u || in->identity_count > 0u) {
            a.comment("identity-row guard");
            for (uint32_t g = 0; g < in->guard_count; g++) {
                const uint32_t k = in->guard_insns[g];
                if (k >= ninsn || !emit(insns[k])) {
                    if (failed_insn != nullptr) {
                        *failed_insn = k;
                    }
                    return SU_NLOW_INTERNAL;
                }
                if (in->guard_checks[g] != 0u) {
                    L.E(Inst::kIdCmp, L.S(insns[k].dst), Imm(0));
                    if (in->guard_checks[g] == 1u) {
                        a.jne(l_body); /* required false, is true */
                    } else {
                        a.je(l_body); /* required true, is false */
                    }
                }
            }
            a.jmp(l_next); /* the row's writable lanes already hold their input values (copied above) */
        }
        a.bind(l_body);

        for (uint32_t k = 0; k < ninsn; k++) {
            const uint32_t op = insns[k].op;
            char note[48];
            std::snprintf(note, sizeof note, "insn %u op %u", k, op);
            a.comment(note);
            if (hoisted[k]) {
                /* Ran before the loop; its source range is that code. */
                ranges.push_back(pre_range[k].first);
                ranges.push_back(pre_range[k].second);
                continue;
            }
            const size_t begin = a.offset();
            if (!emit(insns[k])) {
                if (failed_insn != nullptr) {
                    *failed_insn = k;
                }
                if (msg != nullptr && msg_cap > 0u) {
                    std::snprintf(msg, msg_cap, "opcode %u at insn %u is not lowered", op, k);
                }
                return SU_NLOW_INTERNAL;
            }
            const size_t end = a.offset();
            ranges.push_back(static_cast<uint32_t>(begin));
            ranges.push_back(static_cast<uint32_t>(end));
        }

        a.bind(l_next);
        a.add(r12, 1);
        a.cmp(r12, r13);
        a.jb(l_loop);
        a.xor_(eax, eax); /* before the epilogue, so it has the canonical form add rsp / pops / ret */
        a.add(rsp, static_cast<int32_t>(frame));
        for (uint32_t p = kNSaved; p-- > 0u;) {
            if ((alloc.saved_used >> p) & 1u) {
                a.pop(kSavedPool[p]);
            }
        }
        a.pop(r13);
        a.pop(r12);
        a.pop(rbx);
        a.ret();
        const size_t func_end = a.offset();
        a.bind(l_zero);
        a.xor_(eax, eax);
        a.ret();

        if (a.finalize() != kErrorOk || eh.first != kErrorOk) {
            if (msg != nullptr && msg_cap > 0u) {
                std::snprintf(msg, msg_cap, "asmjit: %s", eh.text);
            }
            return SU_NLOW_INTERNAL;
        }
        if (code.has_unresolved_fixups() || code.flatten() != kErrorOk) {
            return SU_NLOW_INTERNAL;
        }
        const size_t size = code.code_size();
        if (size == 0u) {
            return SU_NLOW_INTERNAL;
        }
        uint8_t *bytes = static_cast<uint8_t *>(std::malloc(size));
        if (bytes == nullptr) {
            return SU_NLOW_RESOURCE;
        }
        if (code.copy_flattened_data(bytes, size) != kErrorOk) {
            std::free(bytes);
            return SU_NLOW_INTERNAL;
        }
        const size_t dlen = logger.data_size();
        char *disasm = static_cast<char *>(std::malloc(dlen + 1u));
        if (disasm == nullptr) {
            std::free(bytes);
            return SU_NLOW_RESOURCE;
        }
        std::memcpy(disasm, logger.data(), dlen);
        disasm[dlen] = '\0';
        uint32_t *rr = nullptr;
        if (!ranges.empty()) {
            rr = static_cast<uint32_t *>(std::malloc(ranges.size() * sizeof *rr));
            if (rr == nullptr) {
                std::free(bytes);
                std::free(disasm);
                return SU_NLOW_RESOURCE;
            }
            std::memcpy(rr, ranges.data(), ranges.size() * sizeof *rr);
        }
        out->bytes = bytes;
        out->size = size;
        out->insn_ranges = rr;
        out->range_count = ninsn;
        out->disasm = disasm;
        out->disasm_len = dlen;
        {
            const std::vector<uint8_t> ub = uw.bytes();
            if (!ub.empty()) {
                out->unwind = static_cast<uint8_t *>(std::malloc(ub.size()));
                if (out->unwind != nullptr) {
                    std::memcpy(out->unwind, ub.data(), ub.size());
                    out->unwind_size = static_cast<uint32_t>(ub.size());
                    out->func_end = static_cast<uint32_t>(func_end);
                }
            }
        }
        return SU_NLOW_OK;
    } catch (const std::bad_alloc &) {
        return SU_NLOW_RESOURCE;
    } catch (...) {
        return SU_NLOW_INTERNAL;
    }
}
