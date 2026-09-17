#ifndef GBA_CORE_THUMB_INC
#define GBA_CORE_THUMB_INC

#include "arm.inc"
#include "bus.inc"
#include "cpu.inc"
#include "state.h"
#include "types.h"

// Thumb-mode decode and execution.
//
// PC model: on entry st.r[15] holds the instruction address + 4, matching the
// pipeline in 16-bit state. Two formats additionally force bit 1 clear when
// reading the PC as a base address, because the ARM7TDMI's word-aligned
// fetch cannot express a halfword-aligned PC.

CORE_FN U32 thumb_pc_base(INOUT(GbaState) st) { return st.r[15] & 0xFFFFFFFCu; }

// Format 1: move shifted register. Format 2 steals the fourth encoding.
CORE_FN void thumb_shifted(INOUT(GbaState) st, U32 op) {
    U32 type = (op >> 11u) & 3u;
    U32 amount = (op >> 6u) & 0x1Fu;
    U32 rs = (op >> 3u) & 7u;
    U32 rd = op & 7u;
    U32 carry = carry_bit(st.cpsr);
    U32 result = shift_by_imm(st.r[rs], type, amount, carry, carry);
    set_nz(st, result);
    set_c(st, carry != 0u);
    st.r[rd] = result;
}

// Format 2: add/subtract register or 3-bit immediate.
CORE_FN void thumb_add_sub(INOUT(GbaState) st, U32 op) {
    bool immediate = ((op >> 10u) & 1u) != 0u;
    bool subtract = ((op >> 9u) & 1u) != 0u;
    U32 field = (op >> 6u) & 7u;
    U32 rs = (op >> 3u) & 7u;
    U32 rd = op & 7u;
    U32 operand = immediate ? field : st.r[field];
    st.r[rd] = subtract ? alu_sub(st, st.r[rs], operand, true)
                        : alu_add(st, st.r[rs], operand, true);
}

// Format 3: move/compare/add/subtract with an 8-bit immediate.
CORE_FN void thumb_imm_op(INOUT(GbaState) st, U32 op) {
    U32 which = (op >> 11u) & 3u;
    U32 rd = (op >> 8u) & 7u;
    U32 imm = op & 0xFFu;
    switch (which) {
        case 0u: st.r[rd] = imm; set_nz(st, imm); break;         // MOV: C and V untouched
        case 1u: alu_sub(st, st.r[rd], imm, true); break;        // CMP
        case 2u: st.r[rd] = alu_add(st, st.r[rd], imm, true); break;
        default: st.r[rd] = alu_sub(st, st.r[rd], imm, true); break;
    }
}

// Format 4: the sixteen register-to-register ALU operations.
CORE_FN void thumb_alu(INOUT(GbaState) st, U32 op) {
    U32 which = (op >> 6u) & 0x0Fu;
    U32 rs = (op >> 3u) & 7u;
    U32 rd = op & 7u;
    U32 a = st.r[rd];
    U32 b = st.r[rs];
    U32 carry = carry_bit(st.cpsr);
    U32 result;

    switch (which) {
        case 0x0u: result = a & b; set_nz(st, result); st.r[rd] = result; break;   // AND
        case 0x1u: result = a ^ b; set_nz(st, result); st.r[rd] = result; break;   // EOR
        case 0x2u:                                                                 // LSL
            result = shift_by_reg(a, 0u, b & 0xFFu, carry, carry);
            set_nz(st, result); set_c(st, carry != 0u); st.r[rd] = result; break;
        case 0x3u:                                                                 // LSR
            result = shift_by_reg(a, 1u, b & 0xFFu, carry, carry);
            set_nz(st, result); set_c(st, carry != 0u); st.r[rd] = result; break;
        case 0x4u:                                                                 // ASR
            result = shift_by_reg(a, 2u, b & 0xFFu, carry, carry);
            set_nz(st, result); set_c(st, carry != 0u); st.r[rd] = result; break;
        case 0x5u: st.r[rd] = alu_adc(st, a, b, true); break;                      // ADC
        case 0x6u: st.r[rd] = alu_sbc(st, a, b, true); break;                      // SBC
        case 0x7u:                                                                 // ROR
            result = shift_by_reg(a, 3u, b & 0xFFu, carry, carry);
            set_nz(st, result); set_c(st, carry != 0u); st.r[rd] = result; break;
        case 0x8u: set_nz(st, a & b); break;                                       // TST
        case 0x9u: st.r[rd] = alu_sub(st, 0u, b, true); break;                     // NEG
        case 0xAu: alu_sub(st, a, b, true); break;                                 // CMP
        case 0xBu: alu_add(st, a, b, true); break;                                 // CMN
        case 0xCu: result = a | b; set_nz(st, result); st.r[rd] = result; break;   // ORR
        case 0xDu: result = a * b; set_nz(st, result); st.r[rd] = result; break;   // MUL
        case 0xEu: result = a & (~b); set_nz(st, result); st.r[rd] = result; break; // BIC
        default:   result = ~b; set_nz(st, result); st.r[rd] = result; break;      // MVN
    }
}

// Format 5: operations that can reach r8-r15, plus BX.
CORE_FN void thumb_hi_reg(INOUT(GbaState) st, U32 op) {
    U32 which = (op >> 8u) & 3u;
    U32 rd = (op & 7u) | ((op >> 4u) & 8u);
    U32 rs = ((op >> 3u) & 7u) | ((op >> 3u) & 8u);

    if (which == 3u) {  // BX
        U32 target = st.r[rs];
        if ((target & 1u) != 0u) st.cpsr = st.cpsr | CPSR_T;
        else                     st.cpsr = st.cpsr & (~CPSR_T);
        cpu_set_reg(st, 15u, target);
        return;
    }
    U32 a = st.r[rd];
    U32 b = st.r[rs];
    if (which == 0u)      cpu_set_reg(st, rd, a + b);   // ADD, no flags
    else if (which == 1u) alu_sub(st, a, b, true);      // CMP, flags only
    else                  cpu_set_reg(st, rd, b);       // MOV, no flags
}

// Format 6: PC-relative load. The PC base is forced word-aligned.
CORE_FN void thumb_pc_load(INOUT(GbaState) st, U32 op) {
    U32 rd = (op >> 8u) & 7u;
    U32 addr = thumb_pc_base(st) + ((op & 0xFFu) * 4u);
    st.r[rd] = bus_read32(st, addr);
}

// Formats 7 and 8: load/store with a register offset, plain or sign-extended.
CORE_FN void thumb_reg_offset(INOUT(GbaState) st, U32 op) {
    U32 ro = (op >> 6u) & 7u;
    U32 rb = (op >> 3u) & 7u;
    U32 rd = op & 7u;
    U32 addr = st.r[rb] + st.r[ro];

    if (((op >> 9u) & 1u) == 0u) {  // format 7
        bool load = ((op >> 11u) & 1u) != 0u;
        bool is_byte = ((op >> 10u) & 1u) != 0u;
        if (load) {
            st.r[rd] = is_byte ? bus_read8(st, addr)
                               : ror32(bus_read32(st, addr), (addr & 3u) * 8u);
        } else {
            if (is_byte) bus_write8(st, addr, st.r[rd]);
            else         bus_write32(st, addr, st.r[rd]);
        }
        return;
    }
    // format 8. Bit 11 is H and bit 10 is S, so the two-bit field reads
    // 0 STRH, 1 LDSB, 2 LDRH, 3 LDSH. These were once swapped, which turned
    // every sign-extended byte load into a halfword load: Pokemon Emerald then
    // looked a map group up as a 16-bit value and crashed entering the truck.
    U32 kind = (op >> 10u) & 3u;  // H:S
    switch (kind) {
        case 0u: bus_write16(st, addr, st.r[rd]); break;                   // STRH
        case 1u: st.r[rd] = sign_extend(bus_read8(st, addr), 8u); break;  // LDSB
        case 2u: {                                                         // LDRH
            U32 v = bus_read16(st, addr);
            st.r[rd] = ((addr & 1u) != 0u) ? ror32(v, 8u) : v;
        } break;
        default:                                                           // LDSH
            st.r[rd] = ((addr & 1u) != 0u) ? sign_extend(bus_read8(st, addr), 8u)
                                           : sign_extend(bus_read16(st, addr), 16u);
            break;
    }
}

// Format 9: load/store with a 5-bit immediate offset, scaled by access width.
CORE_FN void thumb_imm_offset(INOUT(GbaState) st, U32 op) {
    bool is_byte = ((op >> 12u) & 1u) != 0u;
    bool load = ((op >> 11u) & 1u) != 0u;
    U32 offset = (op >> 6u) & 0x1Fu;
    U32 rb = (op >> 3u) & 7u;
    U32 rd = op & 7u;
    U32 addr = st.r[rb] + (is_byte ? offset : (offset * 4u));

    if (load) {
        st.r[rd] = is_byte ? bus_read8(st, addr)
                           : ror32(bus_read32(st, addr), (addr & 3u) * 8u);
    } else {
        if (is_byte) bus_write8(st, addr, st.r[rd]);
        else         bus_write32(st, addr, st.r[rd]);
    }
}

// Format 10: halfword load/store with an immediate offset.
CORE_FN void thumb_halfword(INOUT(GbaState) st, U32 op) {
    bool load = ((op >> 11u) & 1u) != 0u;
    U32 addr = st.r[(op >> 3u) & 7u] + (((op >> 6u) & 0x1Fu) * 2u);
    U32 rd = op & 7u;
    if (load) {
        U32 v = bus_read16(st, addr);
        st.r[rd] = ((addr & 1u) != 0u) ? ror32(v, 8u) : v;
    } else {
        bus_write16(st, addr, st.r[rd]);
    }
}

// Format 11: SP-relative load/store.
CORE_FN void thumb_sp_relative(INOUT(GbaState) st, U32 op) {
    bool load = ((op >> 11u) & 1u) != 0u;
    U32 rd = (op >> 8u) & 7u;
    U32 addr = st.r[13] + ((op & 0xFFu) * 4u);
    if (load) st.r[rd] = ror32(bus_read32(st, addr), (addr & 3u) * 8u);
    else      bus_write32(st, addr, st.r[rd]);
}

// Format 12: compute an address from PC or SP without loading.
CORE_FN void thumb_load_address(INOUT(GbaState) st, U32 op) {
    bool from_sp = ((op >> 11u) & 1u) != 0u;
    U32 rd = (op >> 8u) & 7u;
    U32 offset = (op & 0xFFu) * 4u;
    st.r[rd] = (from_sp ? st.r[13] : thumb_pc_base(st)) + offset;
}

// Format 13: adjust SP by a signed 7-bit word offset.
CORE_FN void thumb_add_sp(INOUT(GbaState) st, U32 op) {
    U32 offset = (op & 0x7Fu) * 4u;
    st.r[13] = ((op >> 7u) & 1u) != 0u ? st.r[13] - offset : st.r[13] + offset;
}

// Format 14: PUSH/POP, always full-descending through SP.
CORE_FN void thumb_push_pop(INOUT(GbaState) st, U32 op) {
    bool load = ((op >> 11u) & 1u) != 0u;
    bool extra = ((op >> 8u) & 1u) != 0u;  // LR on push, PC on pop
    U32 list = op & 0xFFu;

    if (load) {  // POP
        U32 addr = st.r[13];
        for (U32 i = 0u; i < 8u; ++i) {
            if ((list & (1u << i)) == 0u) continue;
            st.r[i] = bus_read32(st, addr);
            addr = addr + 4u;
        }
        if (extra) {
            // On ARMv4T a POP of the PC does not switch instruction set: bit 0
            // of the loaded value is discarded rather than driving the T flag.
            cpu_set_reg(st, 15u, bus_read32(st, addr));
            addr = addr + 4u;
        }
        st.r[13] = addr;
    } else {  // PUSH
        U32 count = popcount32(list) + (extra ? 1u : 0u);
        U32 addr = st.r[13] - count * 4u;
        st.r[13] = addr;
        for (U32 i = 0u; i < 8u; ++i) {
            if ((list & (1u << i)) == 0u) continue;
            bus_write32(st, addr, st.r[i]);
            addr = addr + 4u;
        }
        if (extra) bus_write32(st, addr, st.r[14]);
    }
}

// Format 15: STMIA/LDMIA with writeback.
CORE_FN void thumb_block(INOUT(GbaState) st, U32 op) {
    bool load = ((op >> 11u) & 1u) != 0u;
    U32 rb = (op >> 8u) & 7u;
    U32 list = op & 0xFFu;
    U32 addr = st.r[rb];

    // The same empty-list quirk as ARM: r15 alone is transferred and the base
    // moves by 0x40.
    if (list == 0u) {
        if (load) cpu_set_reg(st, 15u, bus_read32(st, addr));
        else      bus_write32(st, addr, st.r[15] + 2u);
        st.r[rb] = addr + 0x40u;
        return;
    }

    bool base_is_lowest = ((list & (0u - list)) == (1u << rb));
    U32 final_base = addr + popcount32(list) * 4u;
    if (!load && !base_is_lowest) st.r[rb] = final_base;

    for (U32 i = 0u; i < 8u; ++i) {
        if ((list & (1u << i)) == 0u) continue;
        if (load) st.r[i] = bus_read32(st, addr);
        else      bus_write32(st, addr, st.r[i]);
        addr = addr + 4u;
    }
    // A loaded base keeps the loaded value rather than the writeback.
    if (!load || (list & (1u << rb)) == 0u) st.r[rb] = final_base;
}

// Format 16/17: conditional branch, or SWI when the condition field is 0b1111.
CORE_FN void thumb_cond_branch(INOUT(GbaState) st, U32 op) {
    U32 cond = (op >> 8u) & 0x0Fu;
    if (cond == 0x0Fu) {
        if (bios_swi(st, op & 0xFFu)) return;
        cpu_exception(st, VEC_SWI, MODE_SVC, st.r[15] - 2u);
        return;
    }
    if (cond == 0x0Eu) { arm_undefined(st); return; }
    if (!cond_passes(st.cpsr, cond)) return;
    cpu_set_reg(st, 15u, st.r[15] + (sign_extend(op & 0xFFu, 8u) << 1u));
}

// Format 18: unconditional branch.
CORE_FN void thumb_branch(INOUT(GbaState) st, U32 op) {
    cpu_set_reg(st, 15u, st.r[15] + (sign_extend(op & 0x7FFu, 11u) << 1u));
}

// Format 19: long branch with link, assembled from two consecutive halfwords.
// The first stages the high bits of the offset in LR; the second completes the
// branch and leaves LR pointing at the instruction after the pair, with bit 0
// set so that the matching BX returns to Thumb state.
CORE_FN void thumb_long_branch(INOUT(GbaState) st, U32 op) {
    if (((op >> 11u) & 1u) == 0u) {
        st.r[14] = st.r[15] + (sign_extend(op & 0x7FFu, 11u) << 12u);
        return;
    }
    U32 target = st.r[14] + ((op & 0x7FFu) << 1u);
    st.r[14] = (st.r[15] - 2u) | 1u;
    cpu_set_reg(st, 15u, target);
}

// ---------------------------------------------------------------------------

CORE_FN void thumb_execute(INOUT(GbaState) st, U32 op) {
    switch ((op >> 12u) & 0x0Fu) {
        case 0x0u: case 0x1u:
            // 0b000 is the shift group, except that its fourth encoding is
            // add/subtract.
            if (((op >> 11u) & 3u) == 3u) thumb_add_sub(st, op);
            else                          thumb_shifted(st, op);
            return;
        case 0x2u: case 0x3u:
            thumb_imm_op(st, op);
            return;
        case 0x4u:
            if (((op >> 11u) & 1u) != 0u) { thumb_pc_load(st, op); return; }
            if (((op >> 10u) & 1u) != 0u) { thumb_hi_reg(st, op); return; }
            thumb_alu(st, op);
            return;
        case 0x5u:
            thumb_reg_offset(st, op);
            return;
        case 0x6u: case 0x7u:
            thumb_imm_offset(st, op);
            return;
        case 0x8u:
            thumb_halfword(st, op);
            return;
        case 0x9u:
            thumb_sp_relative(st, op);
            return;
        case 0xAu:
            thumb_load_address(st, op);
            return;
        case 0xBu:
            if (((op >> 8u) & 0x0Fu) == 0u) { thumb_add_sp(st, op); return; }
            if (((op >> 9u) & 3u) == 2u)    { thumb_push_pop(st, op); return; }
            arm_undefined(st);
            return;
        case 0xCu:
            thumb_block(st, op);
            return;
        case 0xDu:
            thumb_cond_branch(st, op);
            return;
        case 0xEu:
            if (((op >> 11u) & 1u) != 0u) { arm_undefined(st); return; }  // BLX: ARMv5
            thumb_branch(st, op);
            return;
        default:
            thumb_long_branch(st, op);
            return;
    }
}

#endif  // GBA_CORE_THUMB_INC
