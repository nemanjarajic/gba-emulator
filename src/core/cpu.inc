#ifndef GBA_CORE_CPU_INC
#define GBA_CORE_CPU_INC

#include "bus.inc"
#include "memmap.h"
#include "state.h"
#include "types.h"

// ARM7TDMI substrate: flags, condition codes, register banking, the barrel
// shifter, flag-setting arithmetic, and exception entry. Instruction decode
// lives in arm.inc and thumb.inc.

// ---------------------------------------------------------------------------
// Flags

CORE_FN bool flag_n(U32 cpsr) { return (cpsr & CPSR_N) != 0u; }
CORE_FN bool flag_z(U32 cpsr) { return (cpsr & CPSR_Z) != 0u; }
CORE_FN bool flag_c(U32 cpsr) { return (cpsr & CPSR_C) != 0u; }
CORE_FN bool flag_v(U32 cpsr) { return (cpsr & CPSR_V) != 0u; }

CORE_FN U32 carry_bit(U32 cpsr) { return (cpsr >> 29u) & 1u; }

CORE_FN void set_nz(INOUT(GbaState) st, U32 result) {
    // CPSR_N is bit 31, so the sign bit of the result lands in place unshifted.
    st.cpsr = (st.cpsr & (~(CPSR_N | CPSR_Z))) | (result & CPSR_N) |
              ((result == 0u) ? CPSR_Z : 0u);
}

CORE_FN void set_c(INOUT(GbaState) st, bool c) {
    st.cpsr = c ? (st.cpsr | CPSR_C) : (st.cpsr & (~CPSR_C));
}

CORE_FN void set_v(INOUT(GbaState) st, bool v) {
    st.cpsr = v ? (st.cpsr | CPSR_V) : (st.cpsr & (~CPSR_V));
}

// Condition field in bits 31..28 of every ARM instruction.
CORE_FN bool cond_passes(U32 cpsr, U32 cond) {
    switch (cond) {
        case 0x0u: return flag_z(cpsr);                          // EQ
        case 0x1u: return !flag_z(cpsr);                         // NE
        case 0x2u: return flag_c(cpsr);                          // CS
        case 0x3u: return !flag_c(cpsr);                         // CC
        case 0x4u: return flag_n(cpsr);                          // MI
        case 0x5u: return !flag_n(cpsr);                         // PL
        case 0x6u: return flag_v(cpsr);                          // VS
        case 0x7u: return !flag_v(cpsr);                         // VC
        case 0x8u: return flag_c(cpsr) && !flag_z(cpsr);         // HI
        case 0x9u: return !flag_c(cpsr) || flag_z(cpsr);         // LS
        case 0xAu: return flag_n(cpsr) == flag_v(cpsr);          // GE
        case 0xBu: return flag_n(cpsr) != flag_v(cpsr);          // LT
        case 0xCu: return !flag_z(cpsr) && (flag_n(cpsr) == flag_v(cpsr));  // GT
        case 0xDu: return flag_z(cpsr) || (flag_n(cpsr) != flag_v(cpsr));   // LE
        case 0xEu: return true;                                  // AL
        default:   return false;  // 0xF is NV: never, on ARMv4
    }
}

// ---------------------------------------------------------------------------
// Register banking

// Moves the register file between banks. Call before updating any other CPSR
// bits; it rewrites the mode field itself.
CORE_FN void cpu_set_mode(INOUT(GbaState) st, U32 new_mode) {
    U32 old_bank = mode_to_bank(st.cpsr);
    U32 new_bank = mode_to_bank(new_mode);

    if (old_bank != new_bank) {
        st.bank_r13[old_bank] = st.r[13];
        st.bank_r14[old_bank] = st.r[14];
        st.r[13] = st.bank_r13[new_bank];
        st.r[14] = st.bank_r14[new_bank];

        // FIQ additionally banks r8-r12.
        if (old_bank == BANK_FIQ) {
            for (U32 i = 0u; i < 5u; ++i) {
                st.bank_fiq[i] = st.r[8u + i];
                st.r[8u + i] = st.bank_usr[i];
            }
        } else if (new_bank == BANK_FIQ) {
            for (U32 i = 0u; i < 5u; ++i) {
                st.bank_usr[i] = st.r[8u + i];
                st.r[8u + i] = st.bank_fiq[i];
            }
        }
    }
    st.cpsr = (st.cpsr & (~0x1Fu)) | (new_mode & 0x1Fu);
}

CORE_FN bool mode_has_spsr(U32 cpsr) {
    U32 m = cpsr & 0x1Fu;
    return (m != MODE_USR) && (m != MODE_SYS);
}

// Writes a register, tracking PC writes so the step loop does not also advance.
CORE_FN void cpu_set_reg(INOUT(GbaState) st, U32 rd, U32 val) {
    if (rd == 15u) {
        // Writing the PC forcibly realigns it; the low bits are discarded
        // rather than causing an unaligned fetch.
        st.r[15] = ((st.cpsr & CPSR_T) != 0u) ? (val & 0xFFFFFFFEu) : (val & 0xFFFFFFFCu);
        st.pc_dirty = 1u;
    } else {
        st.r[rd] = val;
    }
}

// Restores CPSR from the current mode's SPSR. Used by exception returns
// (`MOVS pc, lr` and friends).
CORE_FN void cpu_restore_cpsr(INOUT(GbaState) st) {
    if (!mode_has_spsr(st.cpsr)) return;  // no SPSR in USR/SYS; write is ignored
    U32 spsr = st.spsr[mode_to_bank(st.cpsr)];
    cpu_set_mode(st, spsr & 0x1Fu);
    st.cpsr = spsr;
}

// ---------------------------------------------------------------------------
// Barrel shifter
//
// Shift type is bits 6..5: 0 LSL, 1 LSR, 2 ASR, 3 ROR.
//
// The immediate and register forms differ in their zero cases, and those
// differences are exactly what CPU test ROMs probe, so they are two functions
// rather than one with a flag.

// Shift amount came from a 5-bit immediate field. A zero amount is reinterpreted
// per shift type (LSR #0 means #32, ASR #0 means #32, ROR #0 means RRX).
CORE_FN U32 shift_by_imm(U32 val, U32 type, U32 amount, U32 carry_in, OUT(U32) carry_out) {
    if (type == 0u) {  // LSL
        if (amount == 0u) { carry_out = carry_in; return val; }
        carry_out = (val >> (32u - amount)) & 1u;
        return val << amount;
    }
    if (type == 1u) {  // LSR
        if (amount == 0u) { carry_out = val >> 31u; return 0u; }  // LSR #32
        carry_out = (val >> (amount - 1u)) & 1u;
        return val >> amount;
    }
    if (type == 2u) {  // ASR
        if (amount == 0u) {  // ASR #32: every bit becomes the sign bit
            carry_out = val >> 31u;
            return U32(I32(val) >> 31);
        }
        carry_out = (val >> (amount - 1u)) & 1u;
        return U32(I32(val) >> I32(amount));
    }
    // ROR
    if (amount == 0u) {  // RRX: a 33-bit rotate through the carry flag
        carry_out = val & 1u;
        return (carry_in << 31u) | (val >> 1u);
    }
    carry_out = (val >> (amount - 1u)) & 1u;
    return ror32(val, amount);
}

// Shift amount came from the low byte of a register, so it can exceed 31 and a
// zero amount means "no shift" for every type.
CORE_FN U32 shift_by_reg(U32 val, U32 type, U32 amount, U32 carry_in, OUT(U32) carry_out) {
    carry_out = carry_in;
    if (amount == 0u) return val;

    if (type == 0u) {  // LSL
        if (amount < 32u) { carry_out = (val >> (32u - amount)) & 1u; return val << amount; }
        if (amount == 32u) { carry_out = val & 1u; return 0u; }
        carry_out = 0u;
        return 0u;
    }
    if (type == 1u) {  // LSR
        if (amount < 32u) { carry_out = (val >> (amount - 1u)) & 1u; return val >> amount; }
        if (amount == 32u) { carry_out = val >> 31u; return 0u; }
        carry_out = 0u;
        return 0u;
    }
    if (type == 2u) {  // ASR
        if (amount < 32u) {
            carry_out = (val >> (amount - 1u)) & 1u;
            return U32(I32(val) >> I32(amount));
        }
        carry_out = val >> 31u;
        return U32(I32(val) >> 31);
    }
    // ROR: rotating by a multiple of 32 leaves the value alone but still
    // updates the carry from bit 31.
    U32 rot = amount & 31u;
    if (rot == 0u) { carry_out = val >> 31u; return val; }
    carry_out = (val >> (rot - 1u)) & 1u;
    return ror32(val, rot);
}

// ---------------------------------------------------------------------------
// Flag-setting arithmetic.
//
// Written with 32-bit operations only: 64-bit integers are not available in
// GLSL without an extension MoltenVK may not expose.

CORE_FN U32 alu_add(INOUT(GbaState) st, U32 a, U32 b, bool set_flags) {
    U32 r = a + b;
    if (set_flags) {
        set_nz(st, r);
        set_c(st, r < a);  // wrapped, so it carried out
        set_v(st, ((a ^ r) & (b ^ r) & 0x80000000u) != 0u);
    }
    return r;
}

CORE_FN U32 alu_adc(INOUT(GbaState) st, U32 a, U32 b, bool set_flags) {
    U32 cin = carry_bit(st.cpsr);
    U32 r1 = a + b;
    U32 r = r1 + cin;
    if (set_flags) {
        set_nz(st, r);
        set_c(st, (r1 < a) || (r < r1));
        set_v(st, ((a ^ r) & (b ^ r) & 0x80000000u) != 0u);
    }
    return r;
}

CORE_FN U32 alu_sub(INOUT(GbaState) st, U32 a, U32 b, bool set_flags) {
    U32 r = a - b;
    if (set_flags) {
        set_nz(st, r);
        set_c(st, a >= b);  // ARM carry means "no borrow"
        set_v(st, ((a ^ b) & (a ^ r) & 0x80000000u) != 0u);
    }
    return r;
}

CORE_FN U32 alu_sbc(INOUT(GbaState) st, U32 a, U32 b, bool set_flags) {
    U32 borrow = carry_bit(st.cpsr) ^ 1u;
    U32 r1 = a - b;
    U32 r = r1 - borrow;
    if (set_flags) {
        set_nz(st, r);
        set_c(st, (a >= b) && (r1 >= borrow));
        set_v(st, ((a ^ b) & (a ^ r) & 0x80000000u) != 0u);
    }
    return r;
}

// ---------------------------------------------------------------------------
// 32x32 -> 64 multiply, built from 16-bit partial products for the same reason.

CORE_FN void umull32(U32 a, U32 b, OUT(U32) hi, OUT(U32) lo) {
    U32 a0 = a & 0xFFFFu, a1 = a >> 16u;
    U32 b0 = b & 0xFFFFu, b1 = b >> 16u;
    U32 p00 = a0 * b0;
    U32 p01 = a0 * b1;
    U32 p10 = a1 * b0;
    U32 p11 = a1 * b1;
    U32 mid = (p00 >> 16u) + (p01 & 0xFFFFu) + (p10 & 0xFFFFu);
    lo = (p00 & 0xFFFFu) | (mid << 16u);
    hi = p11 + (p01 >> 16u) + (p10 >> 16u) + (mid >> 16u);
}

// Signed variant: compute the unsigned product, then subtract the corrections
// that the two's-complement representation of a negative operand introduces.
CORE_FN void smull32(U32 a, U32 b, OUT(U32) hi, OUT(U32) lo) {
    U32 h, l;
    umull32(a, b, h, l);
    if ((a & 0x80000000u) != 0u) h = h - b;
    if ((b & 0x80000000u) != 0u) h = h - a;
    hi = h;
    lo = l;
}

// 64-bit accumulate: {hi,lo} += {ahi,alo}
CORE_FN void acc64(INOUT(U32) hi, INOUT(U32) lo, U32 ahi, U32 alo) {
    U32 new_lo = lo + alo;
    U32 carry = (new_lo < lo) ? 1u : 0u;
    lo = new_lo;
    hi = hi + ahi + carry;
}

// ---------------------------------------------------------------------------
// Exception entry

KCONST U32 VEC_UNDEFINED = 0x04u;
KCONST U32 VEC_SWI       = 0x08u;
KCONST U32 VEC_IRQ       = 0x18u;

// `return_addr` is what should end up in the new mode's LR.
CORE_FN void cpu_exception(INOUT(GbaState) st, U32 vector, U32 new_mode, U32 return_addr) {
    U32 old_cpsr = st.cpsr;
    cpu_set_mode(st, new_mode);
    st.spsr[mode_to_bank(new_mode)] = old_cpsr;
    st.r[14] = return_addr;
    st.cpsr = st.cpsr | CPSR_I;          // exceptions enter with IRQs masked
    st.cpsr = st.cpsr & (~CPSR_T);       // and always in ARM state
    st.r[15] = vector;
    st.pc_dirty = 1u;
}

#endif  // GBA_CORE_CPU_INC
