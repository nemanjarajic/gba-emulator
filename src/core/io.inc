#ifndef GBA_CORE_IO_INC
#define GBA_CORE_IO_INC

#include "memmap.h"
#include "state.h"
#include "types.h"

// I/O register addresses used by the interrupt controller, DMA, timers and
// input. Offsets are from 0x04000000.

KCONST U32 REG_DISPSTAT = 0x004u;
KCONST U32 REG_VCOUNT   = 0x006u;
KCONST U32 REG_DMA0SAD  = 0x0B0u;  // channels are 12 bytes apart
KCONST U32 REG_TM0CNT_L = 0x100u;  // timers are 4 bytes apart
KCONST U32 REG_KEYINPUT = 0x130u;
KCONST U32 REG_IE       = 0x200u;
KCONST U32 REG_IF       = 0x202u;
KCONST U32 REG_IME      = 0x208u;

// Interrupt source bits, shared by IE and IF.
KCONST U32 IRQ_VBLANK  = 0x0001u;
KCONST U32 IRQ_HBLANK  = 0x0002u;
KCONST U32 IRQ_VCOUNT  = 0x0004u;
KCONST U32 IRQ_TIMER0  = 0x0008u;
KCONST U32 IRQ_DMA0    = 0x0100u;
KCONST U32 IRQ_KEYPAD  = 0x1000u;

// DMA start timing, from bits 12-13 of the control register.
KCONST U32 DMA_NOW     = 0u;
KCONST U32 DMA_VBLANK  = 1u;
KCONST U32 DMA_HBLANK  = 2u;
KCONST U32 DMA_SPECIAL = 3u;

CORE_FN U32 io_get16(INOUT(GbaState) st, U32 off) {
    U32 w = g_io[MEM_IDX(IO_WORDS, st.inst, (off & IO_MASK) >> 2u)];
    return (w >> ((off & 2u) * 8u)) & 0xFFFFu;
}

CORE_FN void io_set16(INOUT(GbaState) st, U32 off, U32 value) {
    U32 idx = MEM_IDX(IO_WORDS, st.inst, (off & IO_MASK) >> 2u);
    U32 sh = (off & 2u) * 8u;
    g_io[idx] = (g_io[idx] & (~(0xFFFFu << sh))) | ((value & 0xFFFFu) << sh);
}

CORE_FN U32 io_get32(INOUT(GbaState) st, U32 off) {
    return g_io[MEM_IDX(IO_WORDS, st.inst, (off & IO_MASK) >> 2u)];
}

// Raises an interrupt source. The CPU only takes it if IME and IE allow and
// the CPSR I bit is clear; until then it sits in IF.
CORE_FN void irq_raise(INOUT(GbaState) st, U32 source) {
    io_set16(st, REG_IF, io_get16(st, REG_IF) | source);
    // A pending, enabled interrupt wakes the CPU from HALT even when IME is
    // clear -- that is how the BIOS IntrWait calls work.
    if ((io_get16(st, REG_IE) & source) != 0u) st.halted = 0u;
}

#endif  // GBA_CORE_IO_INC
