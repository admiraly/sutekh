/* M1-03 AVX2 lowering of SU-LIR through AsmJit (same pinned library as the scalar lowering).
 *
 * Two loops share one frame and one set of column pointers:
 *  - vector loop: 8 rows per iteration in YMM registers, while r12 + 8 <= row_count.
 *  - scalar epilogue: the remaining row_count % 8 rows, one row per iteration, with the M1-01
 *    scalar lowering. Both loops compute the same u32_mod_v1 values.
 *
 * Register plan:
 *  - SU-LIR register r lives in a 32-byte slot at [rsp + 32r]. The frame base is 32-byte aligned.
 *    Vector loop: the slot holds 8 lanes; a bool is an all-ones/zero lane mask. Scalar epilogue:
 *    the slot's first dword holds a u32, a bool is 0 or 1. Each row body defines every register
 *    before use, so the two representations never meet.
 *  - All YMM loads and stores are vmovdqu (no alignment requirement on columns).
 *  - Temporaries: ymm0..ymm3 in the vector loop; eax and ecx in the scalar epilogue and for column
 *    pointers.
 *  - Callee-saved: rbp (frame base), rbx (args), r12 (row index), r13 (row count), r14 (vector row
 *    end). All are pushed on entry and restored on exit.
 *  - Frame: round32(32*R + 8*(inputs+outputs)). Above SU_NATIVE_AVX2_FRAME_LIMIT_BYTES the lowering
 *    returns SU_NLOW_RESOURCE. A frame over one page is probed page by page from the top.
 *  - Zero rows return before any push, after vzeroupper. vzeroupper precedes every ret.
 *  - No calls, no red zone, no xmm state other than the YMM temporaries, MXCSR untouched.
 *
 * Source map: the vector loop body of each instruction (begin/end) is recorded. The scalar
 * epilogue body is part of the artifact but not in the map (see su_native.h). */
#include <asmjit/x86.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "backends/native/avx2/su_native_avx2.h"

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
#else
constexpr Gp kArgReg = rdi;
#endif

constexpr int32_t kSlotBytes = 32;   /* per SU-LIR register, vector and scalar use the same slot */
constexpr int32_t kPageBytes = 4096;

struct LowIns {
    uint32_t op;
    uint32_t dst;
    uint32_t a;
    uint32_t b;
    uint32_t c;
};

struct Ctx {
    std::vector<uint32_t> rord;  /* field table index -> readable ordinal */
    std::vector<uint32_t> word;  /* field table index -> writable ordinal */
    uint32_t nin = 0;
    uint32_t nout = 0;
    int32_t col_base = 0;        /* byte offset of the column pointer array in the frame */
};

/* Column pointer for readable field index f: loaded into rcx. */
void load_col(Assembler &a, const Ctx &c, uint32_t field, bool write) {
    const uint32_t ord = write ? (uint32_t)(c.nin + c.word[field]) : c.rord[field];
    a.mov(rcx, qword_ptr(rsp, c.col_base + static_cast<int32_t>(8u * ord)));
}

/* ---- vector body: 8 rows, ymm temporaries, all accesses vmovdqu -------- */

void emit_vector(Assembler &a, const Ctx &c, const LowIns &ins) {
    const int32_t dst = static_cast<int32_t>(static_cast<uint32_t>(kSlotBytes) * ins.dst);
    const int32_t sa = static_cast<int32_t>(static_cast<uint32_t>(kSlotBytes) * ins.a);
    const int32_t sb = static_cast<int32_t>(static_cast<uint32_t>(kSlotBytes) * ins.b);
    const int32_t sc = static_cast<int32_t>(static_cast<uint32_t>(kSlotBytes) * ins.c);

    switch (ins.op) {
    case 1: /* CONST_U32: value in a */
        a.mov(eax, static_cast<int32_t>(ins.a));
        a.vmovd(xmm0, eax);
        a.vpbroadcastd(ymm0, xmm0);
        a.vmovdqu(ymmword_ptr(rsp, dst), ymm0);
        break;
    case 2: /* LOAD_U32: field index in a */
        load_col(a, c, ins.a, false);
        a.vmovdqu(ymm0, ymmword_ptr(rcx, r12, 2));
        a.vmovdqu(ymmword_ptr(rsp, dst), ymm0);
        break;
    case 3: /* ADD */
        a.vmovdqu(ymm0, ymmword_ptr(rsp, sa));
        a.vmovdqu(ymm1, ymmword_ptr(rsp, sb));
        a.vpaddd(ymm0, ymm0, ymm1);
        a.vmovdqu(ymmword_ptr(rsp, dst), ymm0);
        break;
    case 4: /* SUB */
        a.vmovdqu(ymm0, ymmword_ptr(rsp, sa));
        a.vmovdqu(ymm1, ymmword_ptr(rsp, sb));
        a.vpsubd(ymm0, ymm0, ymm1);
        a.vmovdqu(ymmword_ptr(rsp, dst), ymm0);
        break;
    case 5: /* MUL: low 32 bits, identical for signed and unsigned */
        a.vmovdqu(ymm0, ymmword_ptr(rsp, sa));
        a.vmovdqu(ymm1, ymmword_ptr(rsp, sb));
        a.vpmulld(ymm0, ymm0, ymm1);
        a.vmovdqu(ymmword_ptr(rsp, dst), ymm0);
        break;
    case 6: /* EQ: mask */
        a.vmovdqu(ymm0, ymmword_ptr(rsp, sa));
        a.vmovdqu(ymm1, ymmword_ptr(rsp, sb));
        a.vpcmpeqd(ymm0, ymm0, ymm1);
        a.vmovdqu(ymmword_ptr(rsp, dst), ymm0);
        break;
    case 7: /* LT unsigned: a < b  <=>  (b ^ 0x80000000) > (a ^ 0x80000000) signed */
        a.vmovdqu(ymm0, ymmword_ptr(rsp, sa));
        a.vmovdqu(ymm1, ymmword_ptr(rsp, sb));
        a.vpcmpeqd(ymm3, ymm3, ymm3);
        a.vpslld(ymm3, ymm3, 31);
        a.vpxor(ymm0, ymm0, ymm3);
        a.vpxor(ymm1, ymm1, ymm3);
        a.vpcmpgtd(ymm0, ymm1, ymm0);
        a.vmovdqu(ymmword_ptr(rsp, dst), ymm0);
        break;
    case 8: /* SELECT: cond ? on_true(b) : on_false(c); cond is a mask in slot a */
        a.vmovdqu(ymm0, ymmword_ptr(rsp, sc));
        a.vmovdqu(ymm1, ymmword_ptr(rsp, sb));
        a.vmovdqu(ymm2, ymmword_ptr(rsp, sa));
        a.vpblendvb(ymm0, ymm0, ymm1, ymm2);
        a.vmovdqu(ymmword_ptr(rsp, dst), ymm0);
        break;
    case 9: /* STORE: field index in a, source in b */
        a.vmovdqu(ymm0, ymmword_ptr(rsp, sb));
        load_col(a, c, ins.a, true);
        a.vmovdqu(ymmword_ptr(rcx, r12, 2), ymm0);
        break;
    default:
        break;
    }
}

/* ---- scalar body: one row, same semantics as su_native_asmjit.cpp ------- */

void emit_scalar(Assembler &a, const Ctx &c, const LowIns &ins) {
    const int32_t dst = static_cast<int32_t>(static_cast<uint32_t>(kSlotBytes) * ins.dst);
    const int32_t sa = static_cast<int32_t>(static_cast<uint32_t>(kSlotBytes) * ins.a);
    const int32_t sb = static_cast<int32_t>(static_cast<uint32_t>(kSlotBytes) * ins.b);
    const int32_t sc = static_cast<int32_t>(static_cast<uint32_t>(kSlotBytes) * ins.c);

    switch (ins.op) {
    case 1:
        a.mov(dword_ptr(rsp, dst), static_cast<int32_t>(ins.a));
        break;
    case 2:
        load_col(a, c, ins.a, false);
        a.mov(eax, dword_ptr(rcx, r12, 2));
        a.mov(dword_ptr(rsp, dst), eax);
        break;
    case 3:
        a.mov(eax, dword_ptr(rsp, sa));
        a.add(eax, dword_ptr(rsp, sb));
        a.mov(dword_ptr(rsp, dst), eax);
        break;
    case 4:
        a.mov(eax, dword_ptr(rsp, sa));
        a.sub(eax, dword_ptr(rsp, sb));
        a.mov(dword_ptr(rsp, dst), eax);
        break;
    case 5:
        a.mov(eax, dword_ptr(rsp, sa));
        a.imul(eax, dword_ptr(rsp, sb));
        a.mov(dword_ptr(rsp, dst), eax);
        break;
    case 6:
        a.mov(eax, dword_ptr(rsp, sa));
        a.cmp(eax, dword_ptr(rsp, sb));
        a.sete(al);
        a.movzx(eax, al);
        a.mov(dword_ptr(rsp, dst), eax);
        break;
    case 7:
        a.mov(eax, dword_ptr(rsp, sa));
        a.cmp(eax, dword_ptr(rsp, sb));
        a.setb(al);
        a.movzx(eax, al);
        a.mov(dword_ptr(rsp, dst), eax);
        break;
    case 8:
        a.mov(eax, dword_ptr(rsp, sc));
        a.mov(ecx, dword_ptr(rsp, sb));
        a.cmp(dword_ptr(rsp, sa), 0);
        a.cmovne(eax, ecx);
        a.mov(dword_ptr(rsp, dst), eax);
        break;
    case 9:
        a.mov(eax, dword_ptr(rsp, sb));
        load_col(a, c, ins.a, true);
        a.mov(dword_ptr(rcx, r12, 2), eax);
        break;
    default:
        break;
    }
}

bool op_valid(uint32_t op) {
    return op >= SU_NLOW_OP_CONST_U32 && op <= SU_NLOW_OP_STORE_U32;
}

} // namespace

extern "C" uint32_t su_native_lower_avx2(const SuNativeLowerInput *in, SuNativeLowered *out, char *msg,
                                         size_t msg_cap, uint32_t *failed_insn) {
    if (in == nullptr || out == nullptr) {
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

        Ctx c;
        c.rord.assign(nfields, 0u);
        c.word.assign(nfields, 0u);
        for (uint32_t i = 0; i < nfields; i++) {
            if ((in->field_access[i] & SU_NLOW_ACCESS_READ) != 0u) {
                c.rord[i] = c.nin++;
            }
            if ((in->field_access[i] & SU_NLOW_ACCESS_WRITE) != 0u) {
                c.word[i] = c.nout++;
            }
        }

        const uint64_t col_base = static_cast<uint64_t>(kSlotBytes) * nregs;
        const uint64_t frame_raw = col_base + 8u * static_cast<uint64_t>(c.nin + c.nout);
        const uint64_t frame = (frame_raw + 31u) & ~static_cast<uint64_t>(31u);
        if (frame > static_cast<uint64_t>(SU_NATIVE_AVX2_FRAME_LIMIT_BYTES)) {
            if (msg != nullptr && msg_cap > 0u) {
                std::snprintf(msg, msg_cap, "avx2 frame %llu bytes exceeds the bound",
                              static_cast<unsigned long long>(frame));
            }
            return SU_NLOW_RESOURCE;
        }
        c.col_base = static_cast<int32_t>(col_base);

        std::vector<LowIns> insns(ninsn);
        for (uint32_t k = 0; k < ninsn; k++) {
            const uint32_t *w = in->insn_words + 5u * static_cast<size_t>(k);
            insns[k] = LowIns{w[0], w[1], w[2], w[3], w[4]};
            if (!op_valid(insns[k].op)) {
                if (failed_insn != nullptr) {
                    *failed_insn = k;
                }
                if (msg != nullptr && msg_cap > 0u) {
                    std::snprintf(msg, msg_cap, "unknown opcode at insn %u", k);
                }
                return SU_NLOW_INTERNAL;
            }
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

        std::vector<uint32_t> ranges;
        ranges.reserve(2u * static_cast<size_t>(ninsn));

        Label l_zero = a.new_label();
        Label l_vec = a.new_label();
        Label l_vec_done = a.new_label();
        Label l_sc = a.new_label();
        Label l_sc_done = a.new_label();

        a.cmp(qword_ptr(kArgReg, 8), 0);
        a.je(l_zero);

        a.push(rbp);
        a.push(rbx);
        a.push(r12);
        a.push(r13);
        a.push(r14);
        a.mov(rbx, kArgReg);
        a.mov(rbp, rsp);
        a.sub(rsp, static_cast<int32_t>(frame));
        a.and_(rsp, -32);
        if (frame > static_cast<uint64_t>(kPageBytes)) {
            /* Touch every page of the frame from the top down (slots are written before read). */
            for (int64_t off = static_cast<int64_t>(frame) - 4; off >= 0; off -= kPageBytes) {
                a.mov(dword_ptr(rsp, static_cast<int32_t>(off)), 0);
            }
        }

        a.mov(rax, qword_ptr(rbx, 16));
        for (uint32_t i = 0; i < c.nin; i++) {
            a.mov(rcx, qword_ptr(rax, static_cast<int32_t>(8u * i)));
            a.mov(qword_ptr(rsp, c.col_base + static_cast<int32_t>(8u * i)), rcx);
        }
        a.mov(rax, qword_ptr(rbx, 24));
        for (uint32_t j = 0; j < c.nout; j++) {
            a.mov(rcx, qword_ptr(rax, static_cast<int32_t>(8u * j)));
            a.mov(qword_ptr(rsp, c.col_base + static_cast<int32_t>(8u * (c.nin + j))), rcx);
        }

        a.xor_(r12d, r12d);
        a.mov(r13, qword_ptr(rbx, 8));
        a.mov(r14, r13);
        a.and_(r14, -8);

        /* Vector loop: rows [0, row_count & ~7). */
        a.bind(l_vec);
        a.cmp(r12, r14);
        a.jae(l_vec_done);
        for (uint32_t k = 0; k < ninsn; k++) {
            char note[64];
            std::snprintf(note, sizeof note, "avx2 vec insn %u op %u", k, insns[k].op);
            a.comment(note);
            const size_t begin = a.offset();
            emit_vector(a, c, insns[k]);
            const size_t end = a.offset();
            ranges.push_back(static_cast<uint32_t>(begin));
            ranges.push_back(static_cast<uint32_t>(end));
        }
        a.add(r12, 8);
        a.jmp(l_vec);

        /* Scalar epilogue: the remaining row_count % 8 rows. */
        a.bind(l_vec_done);
        a.cmp(r12, r13);
        a.jae(l_sc_done);
        a.bind(l_sc);
        for (uint32_t k = 0; k < ninsn; k++) {
            emit_scalar(a, c, insns[k]);
        }
        a.add(r12, 1);
        a.cmp(r12, r13);
        a.jb(l_sc);

        a.bind(l_sc_done);
        a.vzeroupper();
        a.mov(rsp, rbp);
        a.pop(r14);
        a.pop(r13);
        a.pop(r12);
        a.pop(rbx);
        a.pop(rbp);
        a.xor_(eax, eax);
        a.ret();

        a.bind(l_zero);
        a.vzeroupper();
        a.xor_(eax, eax);
        a.ret();

        if (a.finalize() != kErrorOk || eh.first != kErrorOk) {
            if (msg != nullptr && msg_cap > 0u) {
                std::snprintf(msg, msg_cap, "asmjit: %s", eh.text);
            }
            return SU_NLOW_INTERNAL;
        }
        if (code.has_unresolved_fixups()) {
            return SU_NLOW_INTERNAL;
        }
        if (code.flatten() != kErrorOk) {
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
