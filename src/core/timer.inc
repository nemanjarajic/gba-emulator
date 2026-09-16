#ifndef GBA_CORE_TIMER_INC
#define GBA_CORE_TIMER_INC

#include "io.inc"
#include "state.h"
#include "types.h"

// The four timers.
//
// Each counts up from its reload value and raises an interrupt on overflow. A
// timer other than timer 0 may instead count the overflows of the one below it.

CORE_FN void timer_tick(INOUT(GbaState) st, U32 cycles) {
    // Called once per emulated instruction, so the case where nothing is
    // running has to cost nothing. Reading four control registers to discover
    // that was four uncoalesced memory accesses per instruction, and it
    // dominated the whole emulator.
    if (st.timer_active == 0u) return;

    U32 previous_overflows = 0u;

    for (U32 t = 0u; t < 4u; ++t) {
        U32 base = REG_TM0CNT_L + t * 4u;
        U32 ctrl = io_get16(st, base + 2u);
        if ((ctrl & 0x0080u) == 0u) {  // disabled
            previous_overflows = 0u;
            continue;
        }

        U32 increments;
        if (t > 0u && (ctrl & 0x0004u) != 0u) {
            increments = previous_overflows;  // cascade
        } else {
            // Prescaler selects 1, 64, 256 or 1024 cycles per increment.
            U32 sel = ctrl & 3u;
            U32 shift = (sel == 0u) ? 0u : (sel == 1u) ? 6u : (sel == 2u) ? 8u : 10u;
            st.timer_prescale[t] = st.timer_prescale[t] + cycles;
            increments = st.timer_prescale[t] >> shift;
            st.timer_prescale[t] = st.timer_prescale[t] - (increments << shift);
        }

        U32 overflows = 0u;
        U32 value = st.timer_counter[t] + increments;
        if (value > 0xFFFFu) {
            // Computed rather than looped: a cascade or a long halt can advance
            // a timer by far more than one period at a time.
            U32 span = 0x10000u - st.timer_reload[t];
            U32 excess = value - 0x10000u;
            overflows = 1u + excess / span;
            value = st.timer_reload[t] + (excess % span);
        }
        st.timer_counter[t] = value & 0xFFFFu;
        // The counter is NOT written back to the I/O array here; the bus read
        // path substitutes it. Storing it every cycle was a memory write per
        // timer per instruction.

        if (overflows != 0u && (ctrl & 0x0040u) != 0u) irq_raise(st, IRQ_TIMER0 << t);
        previous_overflows = overflows;
    }
}

#endif  // GBA_CORE_TIMER_INC
