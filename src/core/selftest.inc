#ifndef GBA_CORE_SELFTEST_INC
#define GBA_CORE_SELFTEST_INC

#include "bus.inc"
#include "state.h"
#include "types.h"

// A fixed sequence of bus traffic whose result is a single word.
//
// This exists to be run on BOTH targets from identical source: the C++ build
// and the GLSL build must produce the same word for the same instance. It is
// the smallest possible version of the M4 differential harness, and it keeps
// the dual-compile guarantee honest from M1 onward rather than taking it on
// faith until the interpreter lands.
//
// It deliberately touches regions with different behaviour: normal RAM, the
// video byte-write quirks, the read-only cartridge, open bus, and the 8-bit
// save bus.
// Shared initialiser, so the two targets cannot drift in their starting state.
CORE_FN void selftest_init(INOUT(GbaState) st, U32 inst) {
    st.inst = inst;
    st.open_bus = 0u;
    st.cycles = 0u;
    st.halted = 0u;
    st.scanline = 0u;
    st.line_cycle = 0u;
    st.cpsr = MODE_SYS;
}

CORE_FN U32 bus_selftest(INOUT(GbaState) st) {
    U32 seed = st.inst * 2654435761u;

    bus_write32(st, 0x02000000u, seed);
    bus_write16(st, 0x03000002u, seed & 0xFFFFu);
    bus_write8(st, 0x06000000u, seed & 0xFFu);   // VRAM BG: byte doubled
    bus_write8(st, 0x07000000u, seed & 0xFFu);   // OAM: dropped
    bus_write8(st, 0x0E000000u, seed & 0xFFu);   // save: 8-bit bus

    U32 acc = 0u;
    acc = acc * 33u + bus_read32(st, 0x02000000u);
    acc = acc * 33u + bus_read16(st, 0x03000002u);
    acc = acc * 33u + bus_read8(st, 0x06000000u);
    acc = acc * 33u + bus_read16(st, 0x06000000u);
    acc = acc * 33u + bus_read32(st, 0x07000000u);
    acc = acc * 33u + bus_read32(st, 0x08000000u);   // cartridge
    acc = acc * 33u + bus_read32(st, 0x08100000u);   // past end of cartridge
    acc = acc * 33u + bus_read32(st, 0x00000000u);   // BIOS
    acc = acc * 33u + bus_read32(st, 0x01000000u);   // open bus
    acc = acc * 33u + bus_read32(st, 0x0E000000u);   // save, byte replicated
    acc = acc * 33u + bus_read32(st, 0x06018000u);   // VRAM odd mirror
    acc = acc * 33u + ror32(acc, 7u);
    acc = acc * 33u + sign_extend(acc, 12u);
    return acc;
}

#endif
