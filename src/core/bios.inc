#ifndef GBA_CORE_BIOS_INC
#define GBA_CORE_BIOS_INC

#include "bus.inc"
#include "cpu.inc"
#include "state.h"
#include "types.h"

// High-level BIOS.
//
// The GBA BIOS is copyrighted and not redistributable, so rather than requiring
// a dump this implements the SWI calls directly in the emulator. An HLE call
// consumes the SWI without taking an exception, so execution simply continues
// at the following instruction.
//
// If a real BIOS image is ever loaded, the SWI path in arm.inc/thumb.inc should
// switch back to cpu_exception; this is the fallback, not a replacement.

// Signed division built from unsigned operations.
//
// The obvious `int(a) / int(b)` is not portable across our two targets: GLSL
// explicitly leaves % undefined when either operand is negative, while C++
// defines it. Doing the sign handling by hand keeps both targets identical,
// which is the entire premise of the shared core.
CORE_FN void sdiv32(U32 num, U32 den, OUT(U32) quot, OUT(U32) rem) {
    bool neg_num = (num & 0x80000000u) != 0u;
    bool neg_den = (den & 0x80000000u) != 0u;
    U32 a = neg_num ? (0u - num) : num;
    U32 b = neg_den ? (0u - den) : den;
    U32 q = a / b;
    U32 r = a - q * b;
    if (neg_num != neg_den) q = 0u - q;
    if (neg_num) r = 0u - r;
    quot = q;
    rem = r;
}

CORE_FN U32 isqrt32(U32 x) {
    U32 res = 0u;
    U32 bit = 1u << 30u;
    while (bit > x) bit = bit >> 2u;
    while (bit != 0u) {
        if (x >= res + bit) {
            x = x - (res + bit);
            res = (res >> 1u) + bit;
        } else {
            res = res >> 1u;
        }
        bit = bit >> 2u;
    }
    return res;
}

// SWI 0x0B CpuSet / SWI 0x0C CpuFastSet.
CORE_FN void bios_cpu_set(INOUT(GbaState) st, bool fast) {
    U32 src = st.r[0];
    U32 dst = st.r[1];
    U32 ctrl = st.r[2];
    U32 count = ctrl & 0x001FFFFFu;
    bool fill = ((ctrl >> 24u) & 1u) != 0u;
    // CpuFastSet is always 32-bit; CpuSet picks width from bit 26.
    bool word = fast || (((ctrl >> 26u) & 1u) != 0u);

    if (word) {
        src = src & 0xFFFFFFFCu;
        dst = dst & 0xFFFFFFFCu;
        U32 value = bus_read32(st, src);
        for (U32 i = 0u; i < count; ++i) {
            if (!fill) value = bus_read32(st, src + i * 4u);
            bus_write32(st, dst + i * 4u, value);
        }
    } else {
        src = src & 0xFFFFFFFEu;
        dst = dst & 0xFFFFFFFEu;
        U32 value = bus_read16(st, src);
        for (U32 i = 0u; i < count; ++i) {
            if (!fill) value = bus_read16(st, src + i * 2u);
            bus_write16(st, dst + i * 2u, value);
        }
    }
}

// Returns true if the call was handled. Unimplemented calls return false so the
// caller can fall back to a real exception rather than silently doing nothing.
CORE_FN bool bios_swi(INOUT(GbaState) st, U32 number) {
    switch (number) {
        case 0x02u:  // Halt
            st.halted = 1u;
            return true;

        case 0x06u: {  // Div: r0 / r1
            if (st.r[1] == 0u) return true;  // hardware is undefined here
            U32 q, r;
            sdiv32(st.r[0], st.r[1], q, r);
            st.r[0] = q;
            st.r[1] = r;
            st.r[3] = (q & 0x80000000u) != 0u ? (0u - q) : q;  // abs(quotient)
            return true;
        }

        case 0x07u: {  // DivArm: operands reversed
            if (st.r[0] == 0u) return true;
            U32 q, r;
            sdiv32(st.r[1], st.r[0], q, r);
            st.r[0] = q;
            st.r[1] = r;
            st.r[3] = (q & 0x80000000u) != 0u ? (0u - q) : q;
            return true;
        }

        case 0x08u:  // Sqrt
            st.r[0] = isqrt32(st.r[0]);
            return true;

        case 0x0Bu:  // CpuSet
            bios_cpu_set(st, false);
            return true;

        case 0x0Cu:  // CpuFastSet
            bios_cpu_set(st, true);
            return true;

        default:
            // SoftReset, the IntrWait family, the affine helpers and the
            // decompressors are not needed by the CPU test ROMs. They arrive
            // with the interrupt controller in M7.
            return false;
    }
}

#endif  // GBA_CORE_BIOS_INC
