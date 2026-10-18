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

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "backends/native/su_native_lower.h"

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

class Lowerer {
public:
    Lowerer(Assembler &as, const std::vector<uint32_t> &rord, const std::vector<uint32_t> &word, uint32_t nin,
            int32_t col_base)
        : a(as), rord_(rord), word_(word), nin_(nin), col_base_(col_base) {}

    Mem S(uint32_t r, uint32_t k = 0) const {
        return qword_ptr(rsp, kShadow + static_cast<int32_t>(r) * kSlot + 8 * static_cast<int32_t>(k));
    }
    Mem SD(uint32_t r) const { return dword_ptr(rsp, kShadow + static_cast<int32_t>(r) * kSlot); }
    int32_t slot_off(uint32_t r) const { return kShadow + static_cast<int32_t>(r) * kSlot; }

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

    void call_helper(uint64_t fn, uint32_t d, uint32_t ra, uint32_t rb, uint32_t rc) {
        a.lea(kCallArg[0], ptr(rsp, slot_off(d)));
        a.lea(kCallArg[1], ptr(rsp, slot_off(ra)));
        a.lea(kCallArg[2], ptr(rsp, slot_off(rb)));
        a.lea(kCallArg[3], ptr(rsp, slot_off(rc)));
        a.mov(rax, fn);
        a.call(rax);
    }

    Assembler &a;

private:
    const std::vector<uint32_t> &rord_;
    const std::vector<uint32_t> &word_;
    uint32_t nin_;
    int32_t col_base_;
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
        const uint64_t frame = (col_base + 8u * static_cast<uint64_t>(nin + nout) + 15u) & ~static_cast<uint64_t>(15u);
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
        Lowerer L(a, rord, word, nin, static_cast<int32_t>(col_base));

        std::vector<uint32_t> ranges;
        ranges.reserve(2u * static_cast<size_t>(ninsn));
        Label l_zero = a.new_label();
        Label l_loop = a.new_label();

        a.cmp(qword_ptr(kArgReg, 8), 0);
        a.je(l_zero);
        a.push(rbx);
        a.push(r12);
        a.push(r13);
        a.mov(rbx, kArgReg);
        a.sub(rsp, static_cast<int32_t>(frame));
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

        /* Emits one instruction; false when the opcode is not lowered. */
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
                        a.mov(eax, L.SD(ra));
                        if (x == SU_NLOW_X_ADD) a.add(eax, L.SD(rb));
                        else if (x == SU_NLOW_X_SUB) a.sub(eax, L.SD(rb));
                        else a.imul(eax, L.SD(rb));
                        a.movsxd(rax, eax);
                    } else if (!q32) {
                        a.mov(rax, L.S(ra));
                        if (x == SU_NLOW_X_ADD) a.add(rax, L.S(rb));
                        else if (x == SU_NLOW_X_SUB) a.sub(rax, L.S(rb));
                        else a.imul(rax, L.S(rb));
                    } else {
                        a.mov(rax, L.S(ra));
                        a.mov(rcx, L.S(rb));
                        if (x == SU_NLOW_X_ADD) L.q_add_rax_rcx();
                        else if (x == SU_NLOW_X_SUB) L.q_sub_rax_rcx();
                        else L.q_mul_rax_rcx();
                    }
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_X_NEG:
                    if (i32) {
                        a.mov(eax, L.SD(ra));
                        a.neg(eax);
                        a.movsxd(rax, eax);
                    } else {
                        a.mov(rax, L.S(ra));
                        if (q32) L.q_neg_rax();
                        else a.neg(rax);
                    }
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_X_ABS:
                    if (i32) {
                        a.mov(eax, L.SD(ra));
                        a.mov(ecx, eax);
                        a.neg(ecx);
                        a.test(eax, eax);
                        a.cmovs(eax, ecx);
                        a.movsxd(rax, eax);
                    } else {
                        a.mov(rax, L.S(ra));
                        a.mov(rcx, rax);
                        a.neg(rcx);
                        if (q32) {
                            a.mov(rdx, INT64_MAX);
                            a.cmovo(rcx, rdx);
                        }
                        a.test(rax, rax);
                        a.cmovs(rax, rcx);
                    }
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_X_MIN:
                    a.mov(rax, L.S(ra));
                    a.mov(rcx, L.S(rb));
                    a.cmp(rcx, rax);
                    a.cmovl(rax, rcx);
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_X_MAX:
                    a.mov(rax, L.S(ra));
                    a.mov(rcx, L.S(rb));
                    a.cmp(rcx, rax);
                    a.cmovg(rax, rcx);
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_X_CLAMP:
                    a.mov(rax, L.S(ra));
                    a.mov(rcx, L.S(rb));
                    a.cmp(rcx, rax);
                    a.cmovg(rax, rcx);
                    a.mov(rcx, L.S(rc));
                    a.cmp(rcx, rax);
                    a.cmovl(rax, rcx);
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_X_EQ:
                case SU_NLOW_X_LT:
                case SU_NLOW_X_LE:
                    a.mov(rax, L.S(ra));
                    a.cmp(rax, L.S(rb));
                    if (x == SU_NLOW_X_EQ) a.sete(al);
                    else if (x == SU_NLOW_X_LT) a.setl(al);
                    else a.setle(al);
                    a.movzx(eax, al);
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_X_SELECT:
                    a.mov(rax, L.S(rc));
                    a.cmp(L.S(ra), 0);
                    a.cmovne(rax, L.S(rb));
                    a.mov(L.S(d), rax);
                    break;
                default:
                    ok = false; /* div, shl, shr come with helpers */
                    break;
                }
            } else {
                switch (op) {
                case SU_NLOW_OP_CONST_U32:
                    a.mov(eax, ra);
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_OP_CONST_I32:
                    a.mov(rax, static_cast<int64_t>(static_cast<int32_t>(ra)));
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_OP_CONST_I64:
                case SU_NLOW_OP_CONST_Q32:
                    a.mov(rax, (static_cast<uint64_t>(rb) << 32) | static_cast<uint64_t>(ra));
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_OP_LOAD_U32:
                    L.in_col(rcx, ra);
                    a.mov(eax, dword_ptr(rcx, r12, 2));
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_OP_LOAD_I32:
                    L.in_col(rcx, ra);
                    a.movsxd(rax, dword_ptr(rcx, r12, 2));
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_OP_LOAD_I64:
                case SU_NLOW_OP_LOAD_Q32:
                    L.load64(ra);
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_OP_LOAD_V3:
                    L.load64(ra);
                    a.mov(L.S(d, 0), rax);
                    L.load64(rb);
                    a.mov(L.S(d, 1), rax);
                    L.load64(rc);
                    a.mov(L.S(d, 2), rax);
                    break;
                case SU_NLOW_OP_STORE_U32:
                case SU_NLOW_OP_STORE_I32:
                    a.mov(eax, L.SD(rb));
                    L.out_col(rcx, ra);
                    a.mov(dword_ptr(rcx, r12, 2), eax);
                    break;
                case SU_NLOW_OP_STORE_I64:
                case SU_NLOW_OP_STORE_Q32:
                    a.mov(rax, L.S(rb));
                    L.store64(ra);
                    break;
                case SU_NLOW_OP_STORE_V3:
                    a.mov(rax, L.S(d, 0));
                    L.store64(ra);
                    a.mov(rax, L.S(d, 1));
                    L.store64(rb);
                    a.mov(rax, L.S(d, 2));
                    L.store64(rc);
                    break;
                case SU_NLOW_OP_ADD_U32:
                case SU_NLOW_OP_SUB_U32:
                case SU_NLOW_OP_MUL_U32:
                    a.mov(eax, L.SD(ra));
                    if (op == SU_NLOW_OP_ADD_U32) a.add(eax, L.SD(rb));
                    else if (op == SU_NLOW_OP_SUB_U32) a.sub(eax, L.SD(rb));
                    else a.imul(eax, L.SD(rb));
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_OP_EQ_U32:
                case SU_NLOW_OP_LT_U32:
                    a.mov(eax, L.SD(ra));
                    a.cmp(eax, L.SD(rb));
                    if (op == SU_NLOW_OP_EQ_U32) a.sete(al);
                    else a.setb(al);
                    a.movzx(eax, al);
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_OP_SELECT_U32:
                    a.mov(rax, L.S(rc));
                    a.cmp(L.S(ra), 0);
                    a.cmovne(rax, L.S(rb));
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_OP_AND_BOOL:
                case SU_NLOW_OP_OR_BOOL:
                    a.mov(rax, L.S(ra));
                    if (op == SU_NLOW_OP_AND_BOOL) a.and_(rax, L.S(rb));
                    else a.or_(rax, L.S(rb));
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_OP_NOT_BOOL:
                    a.mov(rax, L.S(ra));
                    a.xor_(rax, 1);
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_OP_I32_FROM_U32:
                case SU_NLOW_OP_I32_FROM_I64:
                    a.movsxd(rax, L.SD(ra));
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_OP_U32_FROM_I32:
                    a.mov(eax, L.SD(ra));
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_OP_I64_FROM_I32:
                case SU_NLOW_OP_Q32_FROM_RAW:
                case SU_NLOW_OP_Q32_RAW:
                    a.mov(rax, L.S(ra));
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_OP_Q32_FROM_I32:
                    a.mov(rax, L.S(ra));
                    a.shl(rax, 32);
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_OP_Q32_TO_I32_FLOOR:
                case SU_NLOW_OP_Q32_TO_I64_FLOOR:
                    a.mov(rax, L.S(ra));
                    a.sar(rax, 32);
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_OP_MAKE_V3:
                    a.mov(rax, L.S(ra));
                    a.mov(L.S(d, 0), rax);
                    a.mov(rax, L.S(rb));
                    a.mov(L.S(d, 1), rax);
                    a.mov(rax, L.S(rc));
                    a.mov(L.S(d, 2), rax);
                    break;
                case SU_NLOW_OP_V3_GET:
                case SU_NLOW_OP_QUAT_GET:
                    a.mov(rax, L.S(ra, rb));
                    a.mov(L.S(d), rax);
                    break;
                case SU_NLOW_OP_V3_ADD:
                case SU_NLOW_OP_V3_SUB:
                    for (uint32_t c3 = 0; c3 < 3u; c3++) {
                        a.mov(rax, L.S(ra, c3));
                        a.mov(rcx, L.S(rb, c3));
                        if (op == SU_NLOW_OP_V3_ADD) L.q_add_rax_rcx();
                        else L.q_sub_rax_rcx();
                        a.mov(L.S(d, c3), rax);
                    }
                    break;
                case SU_NLOW_OP_V3_SCALE:
                    for (uint32_t c3 = 0; c3 < 3u; c3++) {
                        a.mov(rax, L.S(ra, c3));
                        a.mov(rcx, L.S(rb));
                        L.q_mul_rax_rcx();
                        a.mov(L.S(d, c3), rax);
                    }
                    break;
                case SU_NLOW_OP_V3_NEG:
                    for (uint32_t c3 = 0; c3 < 3u; c3++) {
                        a.mov(rax, L.S(ra, c3));
                        L.q_neg_rax();
                        a.mov(L.S(d, c3), rax);
                    }
                    break;
                case SU_NLOW_OP_SELECT_V3:
                case SU_NLOW_OP_SELECT_QUAT: {
                    const uint32_t n = op == SU_NLOW_OP_SELECT_V3 ? 3u : 4u;
                    a.cmp(L.S(ra), 0);
                    for (uint32_t c3 = 0; c3 < n; c3++) {
                        a.mov(rax, L.S(rc, c3));
                        a.cmovne(rax, L.S(rb, c3)); /* mov does not change flags */
                        a.mov(L.S(d, c3), rax);
                    }
                    break;
                }
                case SU_NLOW_OP_MAKE_QUAT:
                    a.mov(rax, L.S(ra));
                    a.mov(L.S(d, 0), rax);
                    for (uint32_t c3 = 0; c3 < 3u; c3++) {
                        a.mov(rax, L.S(rb, c3));
                        a.mov(L.S(d, c3 + 1u), rax);
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
        Label l_rows = a.new_label();
        (void)l_rows;
        if (in->pre_count > 0u) {
            a.comment("row-invariant instructions");
            for (uint32_t i = 0; i < in->pre_count; i++) {
                const uint32_t k = in->pre_insns[i];
                if (k >= ninsn || !emit(insns[k])) {
                    if (failed_insn != nullptr) {
                        *failed_insn = k;
                    }
                    return SU_NLOW_INTERNAL;
                }
            }
        }
        a.bind(l_loop);
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
                    a.cmp(L.S(insns[k].dst), 0);
                    if (in->guard_checks[g] == 1u) {
                        a.jne(l_body); /* required false, is true */
                    } else {
                        a.je(l_body); /* required true, is false */
                    }
                }
            }
            for (uint32_t i = 0; i < in->identity_count; i++) {
                const uint32_t lane = in->identity_lanes[i];
                L.in_col(rcx, lane);
                a.mov(eax, dword_ptr(rcx, r12, 2));
                L.out_col(rcx, lane);
                a.mov(dword_ptr(rcx, r12, 2), eax);
            }
            a.jmp(l_next);
        }
        a.bind(l_body);

        for (uint32_t k = 0; k < ninsn; k++) {
            const uint32_t op = insns[k].op;
            char note[48];
            std::snprintf(note, sizeof note, "insn %u op %u", k, op);
            a.comment(note);
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
        a.add(rsp, static_cast<int32_t>(frame));
        a.pop(r13);
        a.pop(r12);
        a.pop(rbx);
        a.xor_(eax, eax);
        a.ret();
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
        return SU_NLOW_OK;
    } catch (const std::bad_alloc &) {
        return SU_NLOW_RESOURCE;
    } catch (...) {
        return SU_NLOW_INTERNAL;
    }
}
