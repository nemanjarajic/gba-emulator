#ifndef GBA_CORE_CORE_INC
#define GBA_CORE_CORE_INC

#include "arm.inc"
#include "thumb.inc"
#include "bus.inc"
#include "cpu.inc"
#include "ppu.inc"
#include "state.h"
#include "types.h"

// Scanline timing, and the point at which lines are drawn.
//
// A visible line is rendered as it ends, so it reflects the register and memory
// state the CPU left at that moment. Drawing at end-of-line rather than
// per-pixel is the scanline granularity the plan chose.
CORE_FN void ppu_tick(INOUT(GbaState) st, U32 cycles) {
    st.line_cycle = st.line_cycle + cycles;
    while (st.line_cycle >= CYCLES_PER_SCANLINE) {
        st.line_cycle = st.line_cycle - CYCLES_PER_SCANLINE;
        if (st.scanline < SCREEN_H && (g_flags & FLAG_RENDER) != 0u)
            ppu_render_scanline(st, st.scanline);
        st.scanline = st.scanline + 1u;
        if (st.scanline >= SCANLINES_PER_FRAME) st.scanline = 0u;
    }

    // DISPSTAT (0x04000004) and VCOUNT (0x04000006) share one I/O word.
    U32 idx = MEM_IDX(IO_WORDS, st.inst, 1u);
    U32 dispstat = g_io[idx] & 0xFFFFu;
    dispstat = dispstat & (~0x07u);                       // clear the three status bits
    if (st.scanline >= SCREEN_H) dispstat = dispstat | 0x01u;      // VBlank
    if (st.line_cycle >= 1006u) dispstat = dispstat | 0x02u;       // HBlank
    if (st.scanline == ((dispstat >> 8u) & 0xFFu)) dispstat = dispstat | 0x04u;  // VCount match
    g_io[idx] = (st.scanline << 16u) | dispstat;
}

// The instruction step loop.

CORE_FN void cpu_step(INOUT(GbaState) st) {
    U32 pc = st.r[15];
    st.pc_dirty = 0u;

    if ((st.cpsr & CPSR_T) != 0u) {
        U32 op = bus_read16(st, pc);
        st.r[15] = pc + 4u;  // pipeline-adjusted for the duration of execution
        thumb_execute(st, op);
        if (st.pc_dirty == 0u) st.r[15] = pc + 2u;
    } else {
        U32 op = bus_read32(st, pc);
        st.r[15] = pc + 8u;
        arm_execute(st, op);
        if (st.pc_dirty == 0u) st.r[15] = pc + 4u;
    }

    // Placeholder timing. Real wait states, the prefetch buffer and S/N/I cycle
    // accounting land with the scheduler; the CPU test ROMs do not depend on
    // them, and the learning-first priority puts cycle accuracy out of scope.
    st.cycles = st.cycles + 1u;
    ppu_tick(st, 1u);
}

// Runs until at least `n` cycles have elapsed, or the CPU halts.
//
// The GPU path calls this once per dispatch rather than looping to end-of-frame,
// because macOS will kill a command buffer that runs too long.
CORE_FN void step_cycles(INOUT(GbaState) st, U32 n) {
    U32 target = st.cycles + n;
    while (st.cycles < target) {
        if (st.halted != 0u) break;
        cpu_step(st);
    }
}

#endif  // GBA_CORE_CORE_INC
