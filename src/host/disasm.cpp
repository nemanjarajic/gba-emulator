#include "host/disasm.h"

#include <bit>
#include <cstdio>

namespace gba::host {
namespace {

const char* kCond[16] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                         "hi", "ls", "ge", "lt", "gt", "le", "",   "nv"};
const char* kDataOp[16] = {"and", "eor", "sub", "rsb", "add", "adc", "sbc", "rsc",
                           "tst", "teq", "cmp", "cmn", "orr", "mov", "bic", "mvn"};
const char* kShift[4] = {"lsl", "lsr", "asr", "ror"};

std::string reg(uint32_t r) {
    if (r == 13) return "sp";
    if (r == 14) return "lr";
    if (r == 15) return "pc";
    return "r" + std::to_string(r);
}

std::string hex(uint32_t v) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "0x%X", v);
    return buf;
}

std::string regList(uint32_t list) {
    std::string out = "{";
    bool first = true;
    for (uint32_t i = 0; i < 16; ++i) {
        if (!(list & (1u << i))) continue;
        // Collapse runs: r4-r7 rather than r4,r5,r6,r7.
        uint32_t j = i;
        while (j + 1 < 16 && (list & (1u << (j + 1)))) ++j;
        if (!first) out += ",";
        out += reg(i);
        if (j > i + 1) out += "-" + reg(j);
        else if (j == i + 1) out += "," + reg(j);
        first = false;
        i = j;
    }
    return out + "}";
}

// The shifter operand of a data-processing or load/store instruction.
std::string shifted(uint32_t op) {
    const uint32_t rm = op & 0xF;
    const uint32_t type = (op >> 5) & 3;
    if (op & 0x10) return reg(rm) + ", " + kShift[type] + " " + reg((op >> 8) & 0xF);
    const uint32_t amount = (op >> 7) & 0x1F;
    if (amount == 0) {
        if (type == 0) return reg(rm);
        if (type == 3) return reg(rm) + ", rrx";
        return reg(rm) + ", " + kShift[type] + " #32";
    }
    return reg(rm) + ", " + kShift[type] + " #" + std::to_string(amount);
}

}  // namespace

std::string disasmArm(uint32_t op, uint32_t pc) {
    const std::string c = kCond[op >> 28];

    if ((op & 0x0FFFFFF0) == 0x012FFF10) return "bx" + c + "    " + reg(op & 0xF);

    if ((op & 0x0E000000) == 0x0A000000) {
        const int32_t off = int32_t(op << 8) >> 6;  // sign-extend 24 bits, then <<2
        return std::string(op & 0x01000000 ? "bl" : "b") + c + "     " + hex(pc + 8 + off);
    }

    if ((op & 0x0FC000F0) == 0x00000090) {
        const std::string s = (op & 0x100000) ? "s" : "";
        if (op & 0x200000)
            return "mla" + c + s + "  " + reg((op >> 16) & 0xF) + ", " + reg(op & 0xF) + ", " +
                   reg((op >> 8) & 0xF) + ", " + reg((op >> 12) & 0xF);
        return "mul" + c + s + "  " + reg((op >> 16) & 0xF) + ", " + reg(op & 0xF) + ", " +
               reg((op >> 8) & 0xF);
    }

    if ((op & 0x0F8000F0) == 0x00800090) {
        const char* name = (op & 0x400000) ? ((op & 0x200000) ? "smlal" : "smull")
                                           : ((op & 0x200000) ? "umlal" : "umull");
        return name + c + " " + reg((op >> 12) & 0xF) + ", " + reg((op >> 16) & 0xF) + ", " +
               reg(op & 0xF) + ", " + reg((op >> 8) & 0xF);
    }

    if ((op & 0x0FB00FF0) == 0x01000090)
        return std::string("swp") + c + ((op & 0x400000) ? "b " : "  ") + reg((op >> 12) & 0xF) +
               ", " + reg(op & 0xF) + ", [" + reg((op >> 16) & 0xF) + "]";

    if ((op & 0x0E000090) == 0x00000090 && ((op >> 5) & 3) != 0) {
        const uint32_t sh = (op >> 5) & 3;
        const char* kind = sh == 1 ? "h" : (sh == 2 ? "sb" : "sh");
        const std::string opname = std::string(op & 0x100000 ? "ldr" : "str") + c + kind;
        const uint32_t base = (op >> 16) & 0xF;
        const std::string offset = (op & 0x400000)
                                       ? "#" + std::to_string(((op >> 4) & 0xF0) | (op & 0xF))
                                       : reg(op & 0xF);
        return opname + " " + reg((op >> 12) & 0xF) + ", [" + reg(base) +
               (op & 0x1000000 ? ", " + offset + "]" : "], " + offset);
    }

    if ((op & 0x0C000000) == 0x04000000) {
        const std::string opname = std::string(op & 0x100000 ? "ldr" : "str") + c +
                                   (op & 0x400000 ? "b" : "");
        const uint32_t base = (op >> 16) & 0xF;
        // A negative immediate reads better as #-4 than as -#4.
        const bool down = (op & 0x800000) == 0;
        const std::string offset =
            (op & 0x2000000) ? (std::string(down ? "-" : "") + shifted(op))
                             : ("#" + std::string(down ? "-" : "") + std::to_string(op & 0xFFF));
        return opname + "  " + reg((op >> 12) & 0xF) + ", [" + reg(base) +
               (op & 0x1000000 ? ", " + offset + "]" : "], " + offset) +
               ((op & 0x200000) && (op & 0x1000000) ? "!" : "");
    }

    if ((op & 0x0E000000) == 0x08000000) {
        const char* dir = (op & 0x800000) ? ((op & 0x1000000) ? "ib" : "ia")
                                          : ((op & 0x1000000) ? "db" : "da");
        return std::string(op & 0x100000 ? "ldm" : "stm") + c + dir + " " +
               reg((op >> 16) & 0xF) + ((op & 0x200000) ? "!, " : ", ") + regList(op & 0xFFFF) +
               ((op & 0x400000) ? "^" : "");
    }

    if ((op & 0x0F000000) == 0x0F000000) return "swi" + c + "   " + hex((op >> 16) & 0xFF);

    if ((op & 0x0C000000) == 0x00000000) {
        const uint32_t which = (op >> 21) & 0xF;
        const bool setFlags = (op & 0x100000) != 0;
        // Opcodes 8..B without the S bit are MRS/MSR, not compares.
        if (!setFlags && which >= 8 && which <= 11) {
            const char* psr = (op & 0x400000) ? "spsr" : "cpsr";
            if (op & 0x200000) {
                const std::string src = (op & 0x2000000)
                                            ? "#" + hex(std::rotr(uint32_t(op & 0xFF),
                                                                      int((op >> 8) & 0xF) * 2))
                                            : reg(op & 0xF);
                return "msr" + c + "   " + psr + ", " + src;
            }
            return "mrs" + c + "   " + reg((op >> 12) & 0xF) + ", " + psr;
        }
        const std::string src =
            (op & 0x2000000)
                ? "#" + hex(std::rotr(uint32_t(op & 0xFF), int((op >> 8) & 0xF) * 2))
                : shifted(op);
        const std::string name = std::string(kDataOp[which]) + c + (setFlags ? "s" : "");
        const std::string pad(name.size() >= 6 ? 1 : 6 - name.size(), ' ');
        if (which >= 8 && which <= 11) return name + pad + reg((op >> 16) & 0xF) + ", " + src;
        if (which == 13 || which == 15) return name + pad + reg((op >> 12) & 0xF) + ", " + src;
        return name + pad + reg((op >> 12) & 0xF) + ", " + reg((op >> 16) & 0xF) + ", " + src;
    }

    char buf[32];
    std::snprintf(buf, sizeof buf, "?      %08X", op);
    return buf;
}

std::string disasmThumb(uint16_t op, uint32_t pc) {
    const uint32_t top = op >> 12;

    if (top <= 1) {
        if (((op >> 11) & 3) == 3) {
            const bool imm = (op & 0x400) != 0;
            const std::string rhs = imm ? "#" + std::to_string((op >> 6) & 7) : reg((op >> 6) & 7);
            return std::string((op & 0x200) ? "sub   " : "add   ") + reg(op & 7) + ", " +
                   reg((op >> 3) & 7) + ", " + rhs;
        }
        return std::string(kShift[(op >> 11) & 3]) + "   " + reg(op & 7) + ", " +
               reg((op >> 3) & 7) + ", #" + std::to_string((op >> 6) & 0x1F);
    }
    if (top <= 3) {
        static const char* ops[4] = {"mov", "cmp", "add", "sub"};
        return std::string(ops[(op >> 11) & 3]) + "   " + reg((op >> 8) & 7) + ", #" +
               std::to_string(op & 0xFF);
    }
    if (top == 4) {
        if (op & 0x800)
            return "ldr   " + reg((op >> 8) & 7) + ", [pc, #" + std::to_string((op & 0xFF) * 4) +
                   "]  ; " + hex(((pc + 4) & ~3u) + (op & 0xFF) * 4);
        if (op & 0x400) {
            const uint32_t rd = (op & 7) | ((op >> 4) & 8);
            const uint32_t rs = ((op >> 3) & 7) | ((op >> 3) & 8);
            static const char* hi[4] = {"add", "cmp", "mov", "bx "};
            const uint32_t which = (op >> 8) & 3;
            if (which == 3) return "bx    " + reg(rs);
            return std::string(hi[which]) + "   " + reg(rd) + ", " + reg(rs);
        }
        static const char* alu[16] = {"and", "eor", "lsl", "lsr", "asr", "adc", "sbc", "ror",
                                      "tst", "neg", "cmp", "cmn", "orr", "mul", "bic", "mvn"};
        return std::string(alu[(op >> 6) & 0xF]) + "   " + reg(op & 7) + ", " + reg((op >> 3) & 7);
    }
    if (top == 5) {
        const std::string rd = reg(op & 7), rb = reg((op >> 3) & 7), ro = reg((op >> 6) & 7);
        if (op & 0x200) {
            static const char* k[4] = {"strh ", "ldrsb", "ldrh ", "ldrsh"};
            return std::string(k[(op >> 10) & 3]) + " " + rd + ", [" + rb + ", " + ro + "]";
        }
        const char* k = (op & 0x800) ? ((op & 0x400) ? "ldrb " : "ldr  ")
                                     : ((op & 0x400) ? "strb " : "str  ");
        return std::string(k) + " " + rd + ", [" + rb + ", " + ro + "]";
    }
    if (top <= 7) {
        const bool byte = (op & 0x1000) != 0;
        const uint32_t off = ((op >> 6) & 0x1F) * (byte ? 1 : 4);
        const char* k = (op & 0x800) ? (byte ? "ldrb " : "ldr  ") : (byte ? "strb " : "str  ");
        return std::string(k) + " " + reg(op & 7) + ", [" + reg((op >> 3) & 7) + ", #" +
               std::to_string(off) + "]";
    }
    if (top == 8)
        return std::string((op & 0x800) ? "ldrh  " : "strh  ") + reg(op & 7) + ", [" +
               reg((op >> 3) & 7) + ", #" + std::to_string(((op >> 6) & 0x1F) * 2) + "]";
    if (top == 9)
        return std::string((op & 0x800) ? "ldr   " : "str   ") + reg((op >> 8) & 7) + ", [sp, #" +
               std::to_string((op & 0xFF) * 4) + "]";
    if (top == 0xA)
        return "add   " + reg((op >> 8) & 7) + ", " + std::string((op & 0x800) ? "sp" : "pc") +
               ", #" + std::to_string((op & 0xFF) * 4);
    if (top == 0xB) {
        if (((op >> 8) & 0xF) == 0)
            return std::string("add   sp, #") + ((op & 0x80) ? "-" : "") +
                   std::to_string((op & 0x7F) * 4);
        if (((op >> 9) & 3) == 2) {
            uint32_t list = op & 0xFF;
            if (op & 0x100) list |= (op & 0x800) ? 0x8000 : 0x4000;  // pc on pop, lr on push
            return std::string((op & 0x800) ? "pop   " : "push  ") + regList(list);
        }
    }
    if (top == 0xC)
        return std::string((op & 0x800) ? "ldmia " : "stmia ") + reg((op >> 8) & 7) + "!, " +
               regList(op & 0xFF);
    if (top == 0xD) {
        const uint32_t cond = (op >> 8) & 0xF;
        if (cond == 0xF) return "swi   " + hex(op & 0xFF);
        const int32_t off = int32_t(int8_t(op & 0xFF)) * 2;
        return "b" + std::string(kCond[cond]) + "    " + hex(pc + 4 + off);
    }
    if (top == 0xE && !(op & 0x800)) {
        const int32_t off = (int32_t(op << 21) >> 20);  // sign-extend 11 bits, then <<1
        return "b     " + hex(pc + 4 + off);
    }
    if (top == 0xF)
        return std::string((op & 0x800) ? "bl    (low)" : "bl    (high)");

    char buf[32];
    std::snprintf(buf, sizeof buf, "?     %04X", op);
    return buf;
}

}  // namespace gba::host
