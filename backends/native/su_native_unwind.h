/* SPDX-FileCopyrightText: 2026 admiraly
 * SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
 *
 * Windows x64 unwind data for native kernels (C++, used by the three lowerings). A lowering records each prologue
 * instruction that changes rsp or saves a register, with the code offset just after it, and gets back the bytes of
 * one UNWIND_INFO (version 1, no handler, no chaining) for the kernel's main body. su_native.c places it after the
 * code and registers it with RtlAddFunctionTable, so the system unwinder, debuggers and stack walks can step out
 * of a kernel frame (and out of a helper called from one) to the caller.
 *
 * Kernel shape assumed: [entry test + branch to a leaf tail] pushes, frame allocation (optionally a frame pointer
 * set after the allocation, MSVC style: the epilogue is `lea rsp, [rbp + frame]` or `add rsp, frame`, then the
 * pops, then ret), body, epilogue, then the zero-row tail (xor eax, eax; ret) that never touches the stack and is
 * left out of the function range, so the unwinder treats it as a leaf. Codes are emitted in reverse prologue
 * order as the format requires. */
#ifndef SU_NATIVE_UNWIND_H
#define SU_NATIVE_UNWIND_H

#include <cstddef>
#include <cstdint>
#include <vector>

namespace su_unwind {

enum : uint8_t { kPushNonvol = 0, kAllocLarge = 1, kAllocSmall = 2, kSetFpreg = 3 };

class Builder {
public:
    /* reg: the x86-64 register number (rax 0 ... r15 15), as asmjit's Gp::id(). */
    void push(size_t end, uint32_t reg) { add(end, kPushNonvol, reg, 0u); }
    void alloc(size_t end, uint32_t bytes) { add(end, bytes <= 128u ? kAllocSmall : kAllocLarge, 0u, bytes); }
    /* Frame register = rsp + 0 (FrameOffset 0) established at end. */
    void set_fp(size_t end, uint32_t reg) {
        fp_reg_ = reg;
        add(end, kSetFpreg, 0u, 0u);
    }
    void end_prolog(size_t end) { prolog_ = end; }

    /* UNWIND_INFO bytes, or empty when the prologue does not fit the format (offsets above 255, sizes not a
     * multiple of 8): the kernel is then registered without unwind data. */
    std::vector<uint8_t> bytes() const {
        std::vector<uint8_t> codes;
        std::vector<uint8_t> out;
        if (!ok_ || prolog_ > 255u) {
            return out;
        }
        for (size_t k = ops_.size(); k-- > 0u;) {
            const Op &o = ops_[k];
            if (o.op == kAllocSmall) {
                if (o.size < 8u || o.size % 8u != 0u) {
                    return out;
                }
                codes.push_back(static_cast<uint8_t>(o.end));
                codes.push_back(static_cast<uint8_t>(kAllocSmall | (((o.size - 8u) / 8u) << 4)));
            } else if (o.op == kAllocLarge) {
                if (o.size % 8u != 0u) {
                    return out;
                }
                codes.push_back(static_cast<uint8_t>(o.end));
                if (o.size / 8u <= 0xFFFFu) {
                    codes.push_back(static_cast<uint8_t>(kAllocLarge | (0u << 4)));
                    codes.push_back(static_cast<uint8_t>((o.size / 8u) & 0xFFu));
                    codes.push_back(static_cast<uint8_t>((o.size / 8u) >> 8));
                } else {
                    codes.push_back(static_cast<uint8_t>(kAllocLarge | (1u << 4)));
                    for (uint32_t b = 0; b < 4u; b++) {
                        codes.push_back(static_cast<uint8_t>((o.size >> (8u * b)) & 0xFFu));
                    }
                }
            } else {
                codes.push_back(static_cast<uint8_t>(o.end));
                codes.push_back(static_cast<uint8_t>(o.op | (o.reg << 4)));
            }
        }
        const size_t slots = codes.size() / 2u;
        if (slots > 255u) {
            return std::vector<uint8_t>();
        }
        out.push_back(1u);                                      /* version 1, flags 0 */
        out.push_back(static_cast<uint8_t>(prolog_));           /* SizeOfProlog */
        out.push_back(static_cast<uint8_t>(slots));             /* CountOfCodes */
        out.push_back(static_cast<uint8_t>(fp_reg_ & 0x0Fu));   /* FrameRegister, FrameOffset 0 */
        out.insert(out.end(), codes.begin(), codes.end());
        if (slots % 2u != 0u) {
            out.push_back(0u); /* the code array is padded to an even number of slots */
            out.push_back(0u);
        }
        return out;
    }

private:
    struct Op {
        size_t end;
        uint8_t op;
        uint32_t reg;
        uint32_t size;
    };
    void add(size_t end, uint8_t op, uint32_t reg, uint32_t size) {
        if (end > 255u || reg > 15u) {
            ok_ = false;
        }
        ops_.push_back(Op{end, op, reg, size});
    }
    std::vector<Op> ops_;
    size_t prolog_ = 0;
    uint32_t fp_reg_ = 0;
    bool ok_ = true;
};

} // namespace su_unwind

#endif
