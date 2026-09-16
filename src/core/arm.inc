#ifndef GBA_CORE_ARM_INC
#define GBA_CORE_ARM_INC

#include "bus.inc"
#include "bios.inc"
#include "cpu.inc"
#include "state.h"
#include "types.h"

// ARM-mode instruction decode and execution.
//
// PC model: on entry to arm_execute, st.r[15] holds the address of the
// executing instruction + 8, matching the ARM7TDMI's three-stage pipeline, so
// any read of r15 as an operand sees the architecturally correct value. The
// step loop advances the PC afterwards unless st.pc_dirty says an instruction
// already wrote it.

// Reading r15 as an operand yields instruction+8, except where the shift amount
// comes from a register, which costs an extra cycle and leaves the PC one
// instruction further ahead.
CORE_FN U32 arm_operand(INOUT(GbaState) st, U32 idx, bool pc_ahead) {
    if (idx == 15u && pc_ahead) return st.r[15] + 4u;
    return st.r[idx];
}

// Storing r15 stores instruction+12, which is one word beyond what reading it
// as an ALU operand gives. A genuine hardware asymmetry, and tested for.
CORE_FN U32 arm_stored_reg(INOUT(GbaState) st, U32 idx) {
    return (idx == 15u) ? (st.r[15] + 4u) : st.r[idx];
}

// User-bank access for LDM/STM with the S bit set. Privileged modes can reach
// the user copies of r8-r14 without switching mode.
CORE_FN U32 arm_read_user_reg(INOUT(GbaState) st, U32 i) {
    U32 bank = mode_to_bank(st.cpsr);
    if (bank == BANK_USR) return st.r[i];
    if (i >= 8u && i <= 12u) return (bank == BANK_FIQ) ? st.bank_usr[i - 8u] : st.r[i];
    if (i == 13u) return st.bank_r13[BANK_USR];
    if (i == 14u) return st.bank_r14[BANK_USR];
    return st.r[i];
}

CORE_FN void arm_write_user_reg(INOUT(GbaState) st, U32 i, U32 val) {
    U32 bank = mode_to_bank(st.cpsr);
    if (bank == BANK_USR) { st.r[i] = val; return; }
    if (i >= 8u && i <= 12u) {
        if (bank == BANK_FIQ) st.bank_usr[i - 8u] = val; else st.r[i] = val;
        return;
    }
    if (i == 13u) { st.bank_r13[BANK_USR] = val; return; }
    if (i == 14u) { st.bank_r14[BANK_USR] = val; return; }
    st.r[i] = val;
}

// ---------------------------------------------------------------------------

CORE_FN void arm_data_processing(INOUT(GbaState) st, U32 op) {
    U32 opcode = (op >> 21u) & 0x0Fu;
    bool set_flags = ((op >> 20u) & 1u) != 0u;
    U32 rn = (op >> 16u) & 0x0Fu;
    U32 rd = (op >> 12u) & 0x0Fu;

    U32 shifter_carry = carry_bit(st.cpsr);
    U32 op2;
    bool pc_ahead = false;

    if (((op >> 25u) & 1u) != 0u) {
        // Rotated 8-bit immediate. A rotate of zero leaves the carry alone;
        // any other rotate sets it from the result's top bit.
        U32 imm = op & 0xFFu;
        U32 rot = ((op >> 8u) & 0x0Fu) * 2u;
        if (rot == 0u) {
            op2 = imm;
        } else {
            op2 = ror32(imm, rot);
            shifter_carry = op2 >> 31u;
        }
    } else {
        U32 rm = op & 0x0Fu;
        U32 type = (op >> 5u) & 3u;
        if (((op >> 4u) & 1u) != 0u) {
            // Shift amount from the bottom byte of Rs.
            pc_ahead = true;
            U32 rs = (op >> 8u) & 0x0Fu;
            U32 amount = arm_operand(st, rs, true) & 0xFFu;
            op2 = shift_by_reg(arm_operand(st, rm, true), type, amount, shifter_carry,
                               shifter_carry);
        } else {
            U32 amount = (op >> 7u) & 0x1Fu;
            op2 = shift_by_imm(st.r[rm], type, amount, shifter_carry, shifter_carry);
        }
    }

    U32 a = arm_operand(st, rn, pc_ahead);
    U32 result = 0u;
    bool writes_rd = true;

    // Logical operations take C from the barrel shifter and leave V alone;
    // arithmetic ones compute both. That split is why the shifter carry is
    // threaded through rather than applied up front.
    switch (opcode) {
        case 0x0u: result = a & op2;  if (set_flags) { set_nz(st, result); set_c(st, shifter_carry != 0u); } break;  // AND
        case 0x1u: result = a ^ op2;  if (set_flags) { set_nz(st, result); set_c(st, shifter_carry != 0u); } break;  // EOR
        case 0x2u: result = alu_sub(st, a, op2, set_flags); break;  // SUB
        case 0x3u: result = alu_sub(st, op2, a, set_flags); break;  // RSB
        case 0x4u: result = alu_add(st, a, op2, set_flags); break;  // ADD
        case 0x5u: result = alu_adc(st, a, op2, set_flags); break;  // ADC
        case 0x6u: result = alu_sbc(st, a, op2, set_flags); break;  // SBC
        case 0x7u: result = alu_sbc(st, op2, a, set_flags); break;  // RSC
        case 0x8u: result = a & op2;  set_nz(st, result); set_c(st, shifter_carry != 0u); writes_rd = false; break;  // TST
        case 0x9u: result = a ^ op2;  set_nz(st, result); set_c(st, shifter_carry != 0u); writes_rd = false; break;  // TEQ
        case 0xAu: alu_sub(st, a, op2, true); writes_rd = false; break;  // CMP
        case 0xBu: alu_add(st, a, op2, true); writes_rd = false; break;  // CMN
        case 0xCu: result = a | op2;  if (set_flags) { set_nz(st, result); set_c(st, shifter_carry != 0u); } break;  // ORR
        case 0xDu: result = op2;      if (set_flags) { set_nz(st, result); set_c(st, shifter_carry != 0u); } break;  // MOV
        case 0xEu: result = a & (~op2); if (set_flags) { set_nz(st, result); set_c(st, shifter_carry != 0u); } break;  // BIC
        default:   result = ~op2;     if (set_flags) { set_nz(st, result); set_c(st, shifter_carry != 0u); } break;  // MVN
    }

    // Rd = 15 with S set restores CPSR from SPSR. For most opcodes this is the
    // familiar exception return `<op>S pc, lr, #n`, which also branches.
    //
    // It applies to TST/TEQ/CMP/CMN too, even though they discard their result
    // and do not branch -- the "P variant" inherited from ARMv3. Skipping the
    // restore for those because they have no destination register is wrong, and
    // is exactly what arm.gba test 234 checks.
    if (rd == 15u && set_flags) {
        U32 target = result;
        cpu_restore_cpsr(st);
        if (writes_rd) cpu_set_reg(st, 15u, target);
    } else if (writes_rd) {
        cpu_set_reg(st, rd, result);
    }
}

CORE_FN void arm_psr_transfer(INOUT(GbaState) st, U32 op) {
    bool use_spsr = ((op >> 22u) & 1u) != 0u;
    bool is_msr = ((op >> 21u) & 1u) != 0u;

    if (!is_msr) {  // MRS Rd, <psr>
        U32 rd = (op >> 12u) & 0x0Fu;
        U32 val = (use_spsr && mode_has_spsr(st.cpsr)) ? st.spsr[mode_to_bank(st.cpsr)] : st.cpsr;
        cpu_set_reg(st, rd, val);
        return;
    }

    // MSR <psr>_<fields>, operand
    U32 value;
    if (((op >> 25u) & 1u) != 0u) {
        value = ror32(op & 0xFFu, ((op >> 8u) & 0x0Fu) * 2u);
    } else {
        value = st.r[op & 0x0Fu];
    }

    U32 field = (op >> 16u) & 0x0Fu;
    U32 mask = 0u;
    if ((field & 1u) != 0u) mask = mask | 0x000000FFu;  // control: mode, T, I, F
    if ((field & 2u) != 0u) mask = mask | 0x0000FF00u;
    if ((field & 4u) != 0u) mask = mask | 0x00FF0000u;
    if ((field & 8u) != 0u) mask = mask | 0xFF000000u;  // condition flags

    if (use_spsr) {
        if (!mode_has_spsr(st.cpsr)) return;
        U32 bank = mode_to_bank(st.cpsr);
        st.spsr[bank] = (st.spsr[bank] & (~mask)) | (value & mask);
        return;
    }

    // User mode may only touch the flag byte, whatever the field mask asks for.
    if ((st.cpsr & 0x1Fu) == MODE_USR) mask = mask & 0xFF000000u;

    if ((mask & 0xFFu) != 0u) cpu_set_mode(st, value & 0x1Fu);
    st.cpsr = (st.cpsr & (~mask)) | (value & mask);
}

CORE_FN void arm_multiply(INOUT(GbaState) st, U32 op) {
    bool accumulate = ((op >> 21u) & 1u) != 0u;
    bool set_flags = ((op >> 20u) & 1u) != 0u;
    U32 rd = (op >> 16u) & 0x0Fu;
    U32 rn = (op >> 12u) & 0x0Fu;
    U32 rs = (op >> 8u) & 0x0Fu;
    U32 rm = op & 0x0Fu;

    U32 result = st.r[rm] * st.r[rs];
    if (accumulate) result = result + st.r[rn];
    cpu_set_reg(st, rd, result);
    // ARMv4 leaves C and V alone here; only N and Z are defined.
    if (set_flags) set_nz(st, result);
}

CORE_FN void arm_multiply_long(INOUT(GbaState) st, U32 op) {
    bool is_signed = ((op >> 22u) & 1u) != 0u;
    bool accumulate = ((op >> 21u) & 1u) != 0u;
    bool set_flags = ((op >> 20u) & 1u) != 0u;
    U32 rd_hi = (op >> 16u) & 0x0Fu;
    U32 rd_lo = (op >> 12u) & 0x0Fu;
    U32 rs = (op >> 8u) & 0x0Fu;
    U32 rm = op & 0x0Fu;

    U32 hi, lo;
    if (is_signed) smull32(st.r[rm], st.r[rs], hi, lo);
    else           umull32(st.r[rm], st.r[rs], hi, lo);

    if (accumulate) acc64(hi, lo, st.r[rd_hi], st.r[rd_lo]);

    cpu_set_reg(st, rd_lo, lo);
    cpu_set_reg(st, rd_hi, hi);
    if (set_flags) {
        st.cpsr = (st.cpsr & (~(CPSR_N | CPSR_Z))) | (hi & CPSR_N) |
                  (((hi | lo) == 0u) ? CPSR_Z : 0u);
    }
}

CORE_FN void arm_swap(INOUT(GbaState) st, U32 op) {
    bool is_byte = ((op >> 22u) & 1u) != 0u;
    U32 rn = (op >> 16u) & 0x0Fu;
    U32 rd = (op >> 12u) & 0x0Fu;
    U32 rm = op & 0x0Fu;
    U32 addr = st.r[rn];
    U32 src = st.r[rm];

    if (is_byte) {
        U32 old = bus_read8(st, addr);
        bus_write8(st, addr, src);
        cpu_set_reg(st, rd, old);
    } else {
        // Reads rotate like LDR, but the write is to the aligned address.
        U32 old = ror32(bus_read32(st, addr), (addr & 3u) * 8u);
        bus_write32(st, addr, src);
        cpu_set_reg(st, rd, old);
    }
}

CORE_FN void arm_halfword_transfer(INOUT(GbaState) st, U32 op) {
    bool pre = ((op >> 24u) & 1u) != 0u;
    bool up = ((op >> 23u) & 1u) != 0u;
    bool imm_offset = ((op >> 22u) & 1u) != 0u;
    bool writeback = ((op >> 21u) & 1u) != 0u;
    bool load = ((op >> 20u) & 1u) != 0u;
    U32 rn = (op >> 16u) & 0x0Fu;
    U32 rd = (op >> 12u) & 0x0Fu;
    U32 sh = (op >> 5u) & 3u;

    U32 offset = imm_offset ? (((op >> 4u) & 0xF0u) | (op & 0x0Fu)) : st.r[op & 0x0Fu];
    U32 base = st.r[rn];
    U32 addr = pre ? (up ? base + offset : base - offset) : base;

    if (load) {
        U32 value;
        if (sh == 1u) {
            // Unsigned halfword. An unaligned address rotates rather than faults.
            value = bus_read16(st, addr);
            if ((addr & 1u) != 0u) value = ror32(value, 8u);
        } else if (sh == 2u) {
            value = sign_extend(bus_read8(st, addr), 8u);
        } else {
            // LDRSH from an odd address behaves as a sign-extended byte load.
            if ((addr & 1u) != 0u) value = sign_extend(bus_read8(st, addr), 8u);
            else                   value = sign_extend(bus_read16(st, addr), 16u);
        }
        // Writeback first so that an Rn == Rd collision resolves in favour of
        // the loaded value, as the architecture requires.
        if (!pre) cpu_set_reg(st, rn, up ? base + offset : base - offset);
        else if (writeback) cpu_set_reg(st, rn, addr);
        cpu_set_reg(st, rd, value);
    } else {
        bus_write16(st, addr, arm_stored_reg(st, rd));
        if (!pre) cpu_set_reg(st, rn, up ? base + offset : base - offset);
        else if (writeback) cpu_set_reg(st, rn, addr);
    }
}

CORE_FN void arm_single_transfer(INOUT(GbaState) st, U32 op) {
    bool reg_offset = ((op >> 25u) & 1u) != 0u;  // note: inverted vs data processing
    bool pre = ((op >> 24u) & 1u) != 0u;
    bool up = ((op >> 23u) & 1u) != 0u;
    bool is_byte = ((op >> 22u) & 1u) != 0u;
    bool writeback = ((op >> 21u) & 1u) != 0u;
    bool load = ((op >> 20u) & 1u) != 0u;
    U32 rn = (op >> 16u) & 0x0Fu;
    U32 rd = (op >> 12u) & 0x0Fu;

    U32 offset;
    if (reg_offset) {
        U32 carry = carry_bit(st.cpsr);
        offset = shift_by_imm(st.r[op & 0x0Fu], (op >> 5u) & 3u, (op >> 7u) & 0x1Fu, carry, carry);
    } else {
        offset = op & 0x0FFFu;
    }

    U32 base = st.r[rn];
    U32 addr = pre ? (up ? base + offset : base - offset) : base;

    if (load) {
        U32 value;
        if (is_byte) {
            value = bus_read8(st, addr);
        } else {
            // A word load from a misaligned address returns the aligned word
            // rotated, rather than faulting or returning the straddling bytes.
            value = ror32(bus_read32(st, addr), (addr & 3u) * 8u);
        }
        if (!pre) cpu_set_reg(st, rn, up ? base + offset : base - offset);
        else if (writeback) cpu_set_reg(st, rn, addr);
        cpu_set_reg(st, rd, value);
    } else {
        U32 value = arm_stored_reg(st, rd);
        if (is_byte) bus_write8(st, addr, value);
        else         bus_write32(st, addr, value);
        if (!pre) cpu_set_reg(st, rn, up ? base + offset : base - offset);
        else if (writeback) cpu_set_reg(st, rn, addr);
    }
}

CORE_FN void arm_block_transfer(INOUT(GbaState) st, U32 op) {
    bool pre = ((op >> 24u) & 1u) != 0u;
    bool up = ((op >> 23u) & 1u) != 0u;
    bool psr_bit = ((op >> 22u) & 1u) != 0u;
    bool writeback = ((op >> 21u) & 1u) != 0u;
    bool load = ((op >> 20u) & 1u) != 0u;
    U32 rn = (op >> 16u) & 0x0Fu;
    U32 list = op & 0xFFFFu;

    U32 base = st.r[rn];
    U32 count = popcount32(list);

    // An empty register list is architecturally undefined; the ARM7TDMI
    // transfers r15 alone but adjusts the base as though all sixteen registers
    // had been transferred. Test ROMs check this precisely.
    bool empty = (list == 0u);
    U32 span = empty ? 0x40u : (count * 4u);
    if (empty) list = 0x8000u;

    // The lowest-numbered register always occupies the lowest address, whatever
    // the direction bits say, so walk upwards from a computed start address.
    U32 addr = up ? base : (base - span);
    if (up == pre) addr = addr + 4u;   // pre-increment or post-decrement
    U32 final_base = up ? (base + span) : (base - span);

    // S set with r15 absent means "transfer the user bank"; with r15 present on
    // a load it additionally restores CPSR from SPSR.
    bool r15_in_list = (list & 0x8000u) != 0u;
    bool user_bank = psr_bit && !(load && r15_in_list);

    if (load) {
        // Writeback happens before the loads, so a base register that is also
        // in the list ends up holding the loaded value.
        if (writeback) st.r[rn] = final_base;
        NO_UNROLL for (U32 i = 0u; i < 16u; ++i) {
            if ((list & (1u << i)) == 0u) continue;
            U32 value = bus_read32(st, addr);
            if (user_bank) arm_write_user_reg(st, i, value);
            else           cpu_set_reg(st, i, value);
            addr = addr + 4u;
        }
        if (psr_bit && r15_in_list) cpu_restore_cpsr(st);
    } else {
        // For a store, a base register in the list writes its ORIGINAL value if
        // it is the lowest register present, and its written-back value
        // otherwise -- a genuine quirk, not an implementation shortcut.
        U32 lowest = list & (0u - list);
        bool base_is_lowest = (lowest == (1u << rn));
        if (writeback && !base_is_lowest) st.r[rn] = final_base;

        NO_UNROLL for (U32 i = 0u; i < 16u; ++i) {
            if ((list & (1u << i)) == 0u) continue;
            U32 value = user_bank ? arm_read_user_reg(st, i) : arm_stored_reg(st, i);
            bus_write32(st, addr, value);
            addr = addr + 4u;
        }
        if (writeback) st.r[rn] = final_base;
    }
}

CORE_FN void arm_branch(INOUT(GbaState) st, U32 op) {
    U32 offset = sign_extend(op & 0x00FFFFFFu, 24u) << 2u;
    U32 target = st.r[15] + offset;   // r15 is already instruction+8
    if (((op >> 24u) & 1u) != 0u) st.r[14] = st.r[15] - 4u;  // BL: return address
    cpu_set_reg(st, 15u, target);
}

CORE_FN void arm_bx(INOUT(GbaState) st, U32 op) {
    U32 target = st.r[op & 0x0Fu];
    // Bit 0 of the target selects the instruction set rather than the address.
    if ((target & 1u) != 0u) st.cpsr = st.cpsr | CPSR_T;
    else                     st.cpsr = st.cpsr & (~CPSR_T);
    cpu_set_reg(st, 15u, target);
}

CORE_FN void arm_undefined(INOUT(GbaState) st) {
    cpu_exception(st, VEC_UNDEFINED, MODE_UND, st.r[15] - 4u);
}

// ---------------------------------------------------------------------------

CORE_FN void arm_execute(INOUT(GbaState) st, U32 op) {
    if (!cond_passes(st.cpsr, op >> 28u)) return;

    // Primary decode on bits 27..25. Deliberately a small switch over eight
    // classes rather than a flat 4096-entry table: a table that size takes
    // minutes to compile in glslc and would thrash the GPU instruction cache.
    switch ((op >> 25u) & 7u) {
        case 0u: {
            if ((op & 0x0FFFFFF0u) == 0x012FFF10u) { arm_bx(st, op); return; }
            if ((op & 0x90u) == 0x90u) {
                U32 sh = (op >> 5u) & 3u;
                if (sh != 0u) { arm_halfword_transfer(st, op); return; }
                if ((op & 0x0FC000F0u) == 0x00000090u) { arm_multiply(st, op); return; }
                if ((op & 0x0F8000F0u) == 0x00800090u) { arm_multiply_long(st, op); return; }
                if ((op & 0x0FB00FF0u) == 0x01000090u) { arm_swap(st, op); return; }
                arm_undefined(st);
                return;
            }
            // Opcodes 8..B with S clear are MRS/MSR, not TST/TEQ/CMP/CMN.
            if (((op >> 23u) & 3u) == 2u && ((op >> 20u) & 1u) == 0u) {
                arm_psr_transfer(st, op);
                return;
            }
            arm_data_processing(st, op);
            return;
        }
        case 1u: {
            if (((op >> 23u) & 3u) == 2u && ((op >> 20u) & 1u) == 0u) {
                arm_psr_transfer(st, op);  // MSR with a rotated immediate
                return;
            }
            arm_data_processing(st, op);
            return;
        }
        case 2u:
            arm_single_transfer(st, op);
            return;
        case 3u:
            // Bit 4 set in this class is the architecturally undefined space.
            if ((op & 0x10u) != 0u) { arm_undefined(st); return; }
            arm_single_transfer(st, op);
            return;
        case 4u:
            arm_block_transfer(st, op);
            return;
        case 5u:
            arm_branch(st, op);
            return;
        case 6u:
            arm_undefined(st);  // coprocessor transfers; the GBA has none
            return;
        default:
            if ((op & 0x0F000000u) == 0x0F000000u) {
                // The comment field holds the call number in its top byte.
                if (bios_swi(st, (op >> 16u) & 0xFFu)) return;
                cpu_exception(st, VEC_SWI, MODE_SVC, st.r[15] - 4u);
                return;
            }
            arm_undefined(st);
            return;
    }
}

#endif  // GBA_CORE_ARM_INC
