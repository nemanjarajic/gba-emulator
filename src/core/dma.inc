#ifndef GBA_CORE_DMA_INC
#define GBA_CORE_DMA_INC

#include "bus.inc"
#include "io.inc"
#include "state.h"
#include "types.h"

// Direct memory access.
//
// Transfers run to completion here rather than being interleaved with the CPU.
// Hardware stalls the CPU for the duration anyway, so the visible difference is
// only in timing, which is already scanline granular.

// True when a channel is configured as an audio FIFO feed.
CORE_FN bool dma_is_fifo(INOUT(GbaState) st, U32 ch) {
    if (ch != 1u && ch != 2u) return false;
    U32 dst = io_get32(st, REG_DMA0SAD + ch * 12u + 4u) & 0x0FFFFFFFu;
    return (dst == 0x040000A0u) || (dst == 0x040000A4u);
}

CORE_FN void dma_run(INOUT(GbaState) st, U32 trigger) {
    for (U32 ch = 0u; ch < 4u; ++ch) {
        if ((st.dma_enabled & (1u << ch)) == 0u) continue;

        U32 base = REG_DMA0SAD + ch * 12u;
        U32 ctrl = io_get16(st, base + 10u);
        if ((ctrl & 0x8000u) == 0u) {  // disabled behind our back
            st.dma_enabled = st.dma_enabled & (~(1u << ch));
            continue;
        }
        if (((ctrl >> 12u) & 3u) != trigger) continue;

        bool word = (ctrl & 0x0400u) != 0u;
        U32 step = word ? 4u : 2u;
        U32 dst_ctl = (ctrl >> 5u) & 3u;
        U32 src_ctl = (ctrl >> 7u) & 3u;

        U32 src = st.dma_src[ch];
        U32 dst = st.dma_dst[ch];
        U32 n = st.dma_count[ch];

        // An audio FIFO feed always moves exactly four words to a fixed
        // address, whatever the count and destination-control fields say, and
        // stays armed for the next timer overflow. Without this the channel
        // never drains and a sound driver waiting for buffer space stalls.
        bool fifo = (trigger == DMA_SPECIAL) && dma_is_fifo(st, ch);
        if (fifo) {
            word = true;
            step = 4u;
            dst_ctl = 2u;  // fixed
            n = 4u;
        }

        NO_UNROLL for (U32 i = 0u; i < n; ++i) {
            if (word) bus_write32(st, dst, bus_read32(st, src));
            else      bus_write16(st, dst, bus_read16(st, src));
            if (src_ctl == 0u) src = src + step;
            else if (src_ctl == 1u) src = src - step;
            // src_ctl 2 is fixed; 3 is prohibited and behaves as fixed.
            if (dst_ctl == 0u || dst_ctl == 3u) dst = dst + step;
            else if (dst_ctl == 1u) dst = dst - step;
        }
        st.dma_src[ch] = src;
        st.dma_dst[ch] = dst;

        if ((ctrl & 0x4000u) != 0u) irq_raise(st, IRQ_DMA0 << ch);

        if (fifo) {
            // Source advances, destination does not, and the channel remains
            // enabled for the next overflow.
            st.dma_src[ch] = src;
            continue;
        }

        // A repeating channel stays armed for the next trigger. Destination
        // control 3 additionally reloads the destination, which is how the
        // sound FIFOs and per-frame transfers work.
        if ((ctrl & 0x0200u) != 0u && trigger != DMA_NOW) {
            U32 count = io_get16(st, base + 8u);
            U32 limit = (ch == 3u) ? 0x10000u : 0x4000u;
            st.dma_count[ch] = (count == 0u) ? limit : count;
            if (dst_ctl == 3u)
                st.dma_dst[ch] = io_get32(st, base + 4u) & ((ch == 3u) ? 0x0FFFFFFFu : 0x07FFFFFFu);
        } else {
            io_set16(st, base + 10u, ctrl & 0x7FFFu);  // clear the enable bit
            st.dma_enabled = st.dma_enabled & (~(1u << ch));
        }
    }
}

#endif  // GBA_CORE_DMA_INC
