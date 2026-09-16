#ifndef GBA_CORE_SELFTEST_INC
#define GBA_CORE_SELFTEST_INC

#include "bus.inc"
#include "core.inc"
#include "state.h"
#include "types.h"

// Differential self-tests: fixed work whose result is a single word, run from
// identical source on BOTH targets. The C++ build and the GLSL build must agree
// exactly. This is the M4 harness in miniature, kept alive from M1 onward so
// the dual-compile guarantee cannot rot between milestones.

// Zeroes the whole machine.
//
// This must be exhaustive. C++ zeroes a `GbaState st{}` for free, but GLSL
// leaves a local struct uninitialised, so any field left out here would be
// garbage on the GPU and zero on the CPU -- a divergence in the harness itself
// rather than in the code under test. The banked registers matter most: they
// are invisible until a mode switch reads them.
CORE_FN void selftest_init(INOUT(GbaState) st, U32 inst) {
    st.inst = inst;
    for (U32 i = 0u; i < 16u; ++i) st.r[i] = 0u;
    for (U32 i = 0u; i < NUM_BANKS; ++i) {
        st.spsr[i] = 0u;
        st.bank_r13[i] = 0u;
        st.bank_r14[i] = 0u;
    }
    for (U32 i = 0u; i < 5u; ++i) { st.bank_fiq[i] = 0u; st.bank_usr[i] = 0u; }
    st.cpsr = MODE_SYS;
    st.open_bus = 0u;
    st.pc_dirty = 0u;
    st.cycles = 0u;
    st.halted = 0u;
    st.scanline = 0u;
    st.line_cycle = 0u;
}

// Bus traffic across regions with differing behaviour: normal RAM, the video
// byte-write quirks, the read-only cartridge, open bus, and the 8-bit save bus.
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

// Executes whatever the cartridge happens to contain and hashes the resulting
// machine state.
//
// The caller fills the cartridge with pseudorandom words, so this interprets
// essentially arbitrary instruction streams -- a cheap fuzz test of the decoder
// across both compilers. Divergence here means the C++ and GLSL builds disagree
// about some instruction, which is precisely the failure the architecture has
// to be able to rule out. `step_cycles` bounds the work, so a stream that
// branches to itself or wanders into open bus still terminates.
CORE_FN U32 cpu_selftest(INOUT(GbaState) st, U32 cycles) {
    U32 inst = st.inst;
    selftest_init(st, inst);
    // Seed the registers so different instances take different paths.
    for (U32 i = 0u; i < 15u; ++i) st.r[i] = inst * (i + 1u) * 2654435761u;
    st.r[13] = 0x03007F00u;
    st.r[15] = 0x08000000u;

    step_cycles(st, cycles);

    U32 h = 0u;
    for (U32 i = 0u; i < 16u; ++i) h = h * 33u + st.r[i];
    h = h * 33u + st.cpsr;
    for (U32 i = 0u; i < NUM_BANKS; ++i) {
        h = h * 33u + st.spsr[i];
        h = h * 33u + st.bank_r13[i];
        h = h * 33u + st.bank_r14[i];
    }
    h = h * 33u + st.halted;
    h = h * 33u + st.cycles;
    return h;
}

#endif
