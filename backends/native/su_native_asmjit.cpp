/* M1-01 scalar x86-64 lowering of SU-LIR through AsmJit (pinned, third_party/asmjit.lock.json).
 *
 * The only C++ translation unit of the native backend. It sees the module only through the plain
 * SuNativeLowerInput (backends/native/su_native_lower.h), so no frozen C-only header is included.
 *
 * Plan (no optimizer):
 *  - Register r lives in the 4-byte stack slot [rsp + 4r]. Bool registers hold 0 or 1. The
 *    verifier guarantees types and definition-before-use, so no runtime tags are needed.
 *  - Column base pointers are copied from SuNativeArgs into the frame at entry.
 *  - r12 = row index, r13 = row count, rbx = args pointer. All three are callee-saved, and rbx,
 *    r12 and r13 are the only callee-saved registers the kernel touches; they are pushed first.
 *  - Frame: round16(4*R + 8*(inputs+outputs)) bytes. A frame over one page is probed page by page
 *    from the top, so a guard-page stack never skips a page.
 *  - Zero rows return before any push or stack change.
 *  - Host ABI: SysV passes args in rdi, Win64 in rcx. The Win64 path is emitted but not run here.
 *  - No calls, no red zone, no xmm, no shadow space needed.
 *
 * Code is assembled into a CodeHolder with a plain Environment. JitRuntime is never used. The
 * caller maps the bytes (su_native.c), so no RWX mapping is created here. */
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

/* Records the first AsmJit error instead of aborting. */
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

constexpr int32_t kSlotBytes = 4;
constexpr int32_t kPageBytes = 4096;

struct LowIns {
    uint32_t op;
    uint32_t dst;
    uint32_t a;
    uint32_t b;
    uint32_t c;
};

} // namespace

extern "C" uint32_t su_native_lower_x64(const SuNativeLowerInput *in, SuNativeLowered *out, char *msg,
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

        /* Ordinal of each table index among readable fields and among writable fields. */
        std::vector<uint32_t> rord(nfields, 0u);
        std::vector<uint32_t> word(nfields, 0u);
        uint32_t nin = 0;
        uint32_t nout = 0;
        for (uint32_t i = 0; i < nfields; i++) {
            if ((in->field_access[i] & SU_NLOW_ACCESS_READ) != 0u) {
                rord[i] = nin++;
            }
            if ((in->field_access[i] & SU_NLOW_ACCESS_WRITE) != 0u) {
                word[i] = nout++;
            }
        }

        const uint32_t col_base = (static_cast<uint32_t>(kSlotBytes) * nregs + 7u) & ~7u;
        const uint64_t frame_raw = static_cast<uint64_t>(col_base) + 8u * static_cast<uint64_t>(nin + nout);
        const uint64_t frame = (frame_raw + 15u) & ~static_cast<uint64_t>(15u);
        if (frame > (UINT32_C(1) << 30)) {
            return SU_NLOW_RESOURCE;
        }

        /* Decode and check the instruction words before emitting anything. */
        std::vector<LowIns> insns(ninsn);
        for (uint32_t k = 0; k < ninsn; k++) {
            const uint32_t *w = in->insn_words + 5u * static_cast<size_t>(k);
            insns[k] = LowIns{w[0], w[1], w[2], w[3], w[4]};
            if (insns[k].op < SU_NLOW_OP_CONST_U32 || insns[k].op > SU_NLOW_OP_STORE_U32) {
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
        Label l_loop = a.new_label();

        /* Entry. Zero rows return before anything is saved or touched. */
        a.cmp(qword_ptr(kArgReg, 8), 0);
        a.je(l_zero);

        a.push(rbx);
        a.push(r12);
        a.push(r13);
        a.mov(rbx, kArgReg);
        a.sub(rsp, static_cast<int32_t>(frame));
        if (frame > static_cast<uint64_t>(kPageBytes)) {
            /* Touch every page of the new frame from the top down. The slots written are
             * overwritten before any read (registers are defined before use). */
            for (int64_t off = static_cast<int64_t>(frame) - 4; off >= 0; off -= kPageBytes) {
                a.mov(dword_ptr(rsp, static_cast<int32_t>(off)), 0);
            }
        }

        /* Column base pointers, copied once from the argument arrays. */
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
        a.bind(l_loop);

        for (uint32_t k = 0; k < ninsn; k++) {
            const LowIns &ins = insns[k];
            char note[48];
            std::snprintf(note, sizeof note, "insn %u op %u", k, ins.op);
            a.comment(note);
            const size_t begin = a.offset();
            const int32_t dst = static_cast<int32_t>(static_cast<uint32_t>(kSlotBytes) * ins.dst);
            const int32_t sa = static_cast<int32_t>(static_cast<uint32_t>(kSlotBytes) * ins.a);
            const int32_t sb = static_cast<int32_t>(static_cast<uint32_t>(kSlotBytes) * ins.b);
            const int32_t sc = static_cast<int32_t>(static_cast<uint32_t>(kSlotBytes) * ins.c);

            switch (ins.op) {
            case SU_NLOW_OP_CONST_U32:
                a.mov(dword_ptr(rsp, dst), static_cast<int32_t>(ins.a));
                break;
            case SU_NLOW_OP_LOAD_U32: {
                const int32_t col = static_cast<int32_t>(col_base + 8u * rord[ins.a]);
                a.mov(rcx, qword_ptr(rsp, col));
                a.mov(eax, dword_ptr(rcx, r12, 2));
                a.mov(dword_ptr(rsp, dst), eax);
                break;
            }
            case SU_NLOW_OP_ADD_U32:
                a.mov(eax, dword_ptr(rsp, sa));
                a.add(eax, dword_ptr(rsp, sb));
                a.mov(dword_ptr(rsp, dst), eax);
                break;
            case SU_NLOW_OP_SUB_U32:
                a.mov(eax, dword_ptr(rsp, sa));
                a.sub(eax, dword_ptr(rsp, sb));
                a.mov(dword_ptr(rsp, dst), eax);
                break;
            case SU_NLOW_OP_MUL_U32:
                /* The low 32 bits of the signed and unsigned products are identical. */
                a.mov(eax, dword_ptr(rsp, sa));
                a.imul(eax, dword_ptr(rsp, sb));
                a.mov(dword_ptr(rsp, dst), eax);
                break;
            case SU_NLOW_OP_EQ_U32:
                a.mov(eax, dword_ptr(rsp, sa));
                a.cmp(eax, dword_ptr(rsp, sb));
                a.sete(al);
                a.movzx(eax, al);
                a.mov(dword_ptr(rsp, dst), eax);
                break;
            case SU_NLOW_OP_LT_U32:
                /* Unsigned less-than: setb (CF). */
                a.mov(eax, dword_ptr(rsp, sa));
                a.cmp(eax, dword_ptr(rsp, sb));
                a.setb(al);
                a.movzx(eax, al);
                a.mov(dword_ptr(rsp, dst), eax);
                break;
            case SU_NLOW_OP_SELECT_U32:
                /* cond is a bool slot (0 or 1). on_false is the default value. */
                a.mov(eax, dword_ptr(rsp, sc));
                a.mov(ecx, dword_ptr(rsp, sb));
                a.cmp(dword_ptr(rsp, sa), 0);
                a.cmovne(eax, ecx);
                a.mov(dword_ptr(rsp, dst), eax);
                break;
            case SU_NLOW_OP_STORE_U32: {
                const int32_t col = static_cast<int32_t>(col_base + 8u * (nin + word[ins.a]));
                a.mov(eax, dword_ptr(rsp, sb));
                a.mov(rcx, qword_ptr(rsp, col));
                a.mov(dword_ptr(rcx, r12, 2), eax);
                break;
            }
            default:
                return SU_NLOW_INTERNAL;
            }

            const size_t end = a.offset();
            ranges.push_back(static_cast<uint32_t>(begin));
            ranges.push_back(static_cast<uint32_t>(end));
        }

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
