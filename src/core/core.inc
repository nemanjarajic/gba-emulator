#ifndef GBA_CORE_CORE_INC
#define GBA_CORE_CORE_INC

#include "arm.inc"
#include "bus.inc"
#include "cpu.inc"
#include "dma.inc"
#include "io.inc"
#include "ppu.inc"
#include "state.h"
#include "thumb.inc"
#include "timer.inc"
#include "types.h"

// The scheduler and the instruction step loop.

// HBlank begins partway through each scanline; the line is 1232 cycles long.
KCONST U32 HBLANK_START = 1006u;

// Advances every clocked subsystem by `cycles`.
CORE_FN void scheduler_tick(INOUT(GbaState) st, U32 cycles) {
    timer_tick(st, cycles);

    U32 before = st.line_cycle;
    st.line_cycle = st.line_cycle + cycles;

    // HBlank, which fires on visible lines and drives HBlank DMA.
    if (before < HBLANK_START && st.line_cycle >= HBLANK_START) {
        U32 dispstat = io_get16(st, REG_DISPSTAT) | 0x02u;
        io_set16(st, REG_DISPSTAT, dispstat);
        if ((dispstat & 0x10u) != 0u) irq_raise(st, IRQ_HBLANK);
        if (st.scanline < SCREEN_H) dma_run(st, DMA_HBLANK);
    }

    while (st.line_cycle >= CYCLES_PER_SCANLINE) {
        st.line_cycle = st.line_cycle - CYCLES_PER_SCANLINE;

        // A visible line is drawn as it ends, against the state the CPU left.
        if (st.scanline < SCREEN_H && (g_flags & FLAG_RENDER) != 0u)
            ppu_render_scanline(st, st.scanline);

        st.scanline = st.scanline + 1u;
        if (st.scanline >= SCANLINES_PER_FRAME) st.scanline = 0u;

        U32 dispstat = io_get16(st, REG_DISPSTAT) & (~0x07u);
        if (st.scanline >= SCREEN_H) dispstat = dispstat | 0x01u;              // VBlank
        if (st.scanline == ((dispstat >> 8u) & 0xFFu)) dispstat = dispstat | 0x04u;
        io_set16(st, REG_DISPSTAT, dispstat);
        io_set16(st, REG_VCOUNT, st.scanline);

        if (st.scanline == SCREEN_H) {  // just entered VBlank
            if ((dispstat & 0x08u) != 0u) irq_raise(st, IRQ_VBLANK);
            dma_run(st, DMA_VBLANK);
        }
        if ((dispstat & 0x04u) != 0u && (dispstat & 0x20u) != 0u) irq_raise(st, IRQ_VCOUNT);

        // Keypad interrupts are level triggered, so the condition is
        // re-evaluated rather than fired on a change. Once a scanline is often
        // enough and costs one register read.
        keypad_check(st);
    }
}

// Takes a pending interrupt if one is enabled and not masked.
CORE_FN void irq_check(INOUT(GbaState) st) {
    // Cached rather than read from I/O: this runs between every pair of
    // instructions, and the load was one uncoalesced access per instruction.
    if (st.irq_ready == 0u) return;

    // An enabled interrupt wakes the CPU from HALT even when IME is clear.
    // That is what makes the BIOS IntrWait calls terminate.
    st.halted = 0u;

    if ((io_get16(st, REG_IME) & 1u) == 0u) return;
    if ((st.cpsr & CPSR_I) != 0u) return;

    // The BIOS handler returns with `subs pc, lr, #4`, so LR must hold the
    // address of the interrupted instruction plus four.
    cpu_exception(st, VEC_IRQ, MODE_IRQ, st.r[15] + 4u);
}

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

    // A channel armed with immediate timing runs as soon as the store that
    // enabled it has finished, never from inside it.
    if (st.dma_enabled != 0u) dma_run(st, DMA_NOW);

    // Placeholder timing. Real wait states, the prefetch buffer and S/N/I cycle
    // accounting would go here; the learning-first priority puts cycle accuracy
    // out of scope, and no test ROM depends on it.
    st.cycles = st.cycles + 1u;
    scheduler_tick(st, 1u);
}

// Runs at least `n` cycles.
//
// The GPU path calls this once per dispatch rather than looping to end of
// frame, because macOS will kill a command buffer that runs too long.
CORE_FN void step_cycles(INOUT(GbaState) st, U32 n) {
    U32 target = st.cycles + n;
    while (st.cycles < target) {
        irq_check(st);

        if (st.halted != 0u) {
            // Halted, but the clock must keep running or the interrupt that
            // would wake us could never arrive. Skip to the end of the scanline
            // instead of crawling: a vblank wait is around 200,000 cycles, and
            // stepping those one at a time would dominate every dispatch.
            U32 remaining = target - st.cycles;
            U32 to_line_end = CYCLES_PER_SCANLINE - st.line_cycle;
            U32 skip = (remaining < to_line_end) ? remaining : to_line_end;
            if (skip == 0u) skip = 1u;
            st.cycles = st.cycles + skip;
            scheduler_tick(st, skip);
            continue;
        }

        cpu_step(st);
    }
}

#endif  // GBA_CORE_CORE_INC
