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
KCONST U32 REG_KEYCNT   = 0x132u;
KCONST U32 REG_SOUNDCNT_H = 0x082u;
KCONST U32 REG_FIFO_A   = 0x0A0u;
KCONST U32 REG_FIFO_B   = 0x0A4u;
KCONST U32 REG_IE       = 0x200u;
KCONST U32 REG_IF       = 0x202u;
KCONST U32 REG_IME      = 0x208u;

// Interrupt source bits, shared by IE and IF.
KCONST U32 IRQ_VBLANK  = 0x0001u;
KCONST U32 IRQ_HBLANK  = 0x0002u;
KCONST U32 IRQ_VCOUNT  = 0x0004u;
KCONST U32 IRQ_TIMER0  = 0x0008u;
KCONST U32 IRQ_SERIAL  = 0x0080u;  // defined for completeness; never raised
KCONST U32 IRQ_DMA0    = 0x0100u;
KCONST U32 IRQ_KEYPAD  = 0x1000u;
KCONST U32 IRQ_GAMEPAK = 0x2000u;  // defined for completeness; never raised

// The SDK's interrupt handler records served interrupts here, and the BIOS
// IntrWait calls read it back. Reached through the IWRAM mirror at
// 0x03FFFFF8, which is where the stock BIOS looks.
KCONST U32 BIOS_INTR_FLAGS = 0x03FFFFF8u;

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

// Recomputes the cached "an enabled interrupt is pending" flag. Must be called
// wherever IE or IF changes.
CORE_FN void irq_update(INOUT(GbaState) st) {
    U32 ie_if = io_get32(st, REG_IE);  // IE in the low half, IF in the high half
    st.irq_ready = (((ie_if & 0xFFFFu) & (ie_if >> 16u)) != 0u) ? 1u : 0u;
}

// Raises an interrupt source. The CPU only takes it if IME and IE allow and
// the CPSR I bit is clear; until then it sits in IF.
CORE_FN void irq_raise(INOUT(GbaState) st, U32 source) {
    io_set16(st, REG_IF, io_get16(st, REG_IF) | source);
    irq_update(st);
    // A pending, enabled interrupt wakes the CPU from HALT even when IME is
    // clear -- that is how the BIOS IntrWait calls work.
    if (st.irq_ready != 0u) st.halted = 0u;
}

// Keypad interrupts are level triggered: the condition is re-evaluated against
// the current controller state rather than fired on a transition.
CORE_FN void keypad_check(INOUT(GbaState) st) {
    U32 keycnt = io_get16(st, REG_KEYCNT);
    if ((keycnt & 0x4000u) == 0u) return;  // interrupt not enabled
    U32 mask = keycnt & 0x03FFu;
    if (mask == 0u) return;

    U32 held = (~io_get16(st, REG_KEYINPUT)) & 0x03FFu;  // KEYINPUT is active low
    // Bit 15 picks the condition: set means every selected key must be held,
    // clear means any one of them.
    bool fire = ((keycnt & 0x8000u) != 0u) ? ((held & mask) == mask) : ((held & mask) != 0u);
    if (fire) irq_raise(st, IRQ_KEYPAD);
}

#endif  // GBA_CORE_IO_INC
