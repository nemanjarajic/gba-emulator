#ifndef GBA_CORE_BUS_INC
#define GBA_CORE_BUS_INC

#include "memmap.h"
#include "state.h"
#include "types.h"

// The memory bus.
//
// Storage is uint[] everywhere, so byte and halfword access is shift-and-mask
// over the containing word. GLSL cannot pass a storage buffer to a function,
// which is why each region's load and store is written out inline rather than
// factored into a helper taking the array. The repetition is deliberate.

CORE_FN U32 merge_field(U32 old_word, U32 mask, U32 ins) {
    return (old_word & (~mask)) | (ins & mask);
}

// Reads past the end of the cartridge return the low halfword of addr/2 rather
// than zero, and a few commercial games depend on it.
CORE_FN U32 rom_open_bus(U32 a) {
    U32 lo = (a >> 1u) & 0xFFFFu;
    U32 hi = ((a >> 1u) + 1u) & 0xFFFFu;
    return lo | (hi << 16u);
}

// ---------------------------------------------------------------------------
// Save memory sits on an 8-bit bus: wider accesses see the addressed byte
// replicated, and wider writes store only the low byte.

CORE_FN U32 sram_read8(INOUT(GbaState) st, U32 addr) {
    U32 off = addr & (SRAM_SIZE - 1u);
    U32 w = g_sram[MEM_IDX(SRAM_WORDS, st.inst, off >> 2u)];
    return (w >> ((off & 3u) * 8u)) & 0xFFu;
}

CORE_FN void sram_write8(INOUT(GbaState) st, U32 addr, U32 val) {
    U32 off = addr & (SRAM_SIZE - 1u);
    U32 idx = MEM_IDX(SRAM_WORDS, st.inst, off >> 2u);
    U32 sh = (off & 3u) * 8u;
    g_sram[idx] = merge_field(g_sram[idx], 0xFFu << sh, (val & 0xFFu) << sh);
}

// ---------------------------------------------------------------------------
// Aligned word load for every region except save memory.

CORE_FN U32 bus_read_word_aligned(INOUT(GbaState) st, U32 addr) {
    U32 a = addr & 0xFFFFFFFCu;
    U32 region = (a >> 24u) & 0x0Fu;
    switch (region) {
        case 0x00u:
            // Only the low 16 KiB is BIOS; above that, and all of region 1, is
            // unmapped. (Reads of the BIOS from outside it are further
            // restricted on hardware -- see M2.)
            if (a < BIOS_SIZE) return g_bios[a >> 2u];
            return st.open_bus;
        case 0x02u:
            return g_ewram[MEM_IDX(EWRAM_WORDS, st.inst, (a & EWRAM_MASK) >> 2u)];
        case 0x03u:
            return g_iwram[MEM_IDX(IWRAM_WORDS, st.inst, (a & IWRAM_MASK) >> 2u)];
        case 0x04u: {
            U32 off = a & 0x00FFFFFFu;
            if (off >= IO_SIZE) return st.open_bus;  // M7: 0x04xx0800 mirror
            return g_io[MEM_IDX(IO_WORDS, st.inst, off >> 2u)];
        }
        case 0x05u:
            return g_pram[MEM_IDX(PRAM_WORDS, st.inst, (a & PRAM_MASK) >> 2u)];
        case 0x06u:
            return g_vram[MEM_IDX(VRAM_WORDS, st.inst, vram_offset(a) >> 2u)];
        case 0x07u:
            return g_oam[MEM_IDX(OAM_WORDS, st.inst, (a & OAM_MASK) >> 2u)];
        // The cartridge is visible three times, at 0x08/0x0A/0x0C, differing
        // only in wait-state configuration (which M2 will model).
        case 0x08u: case 0x09u: case 0x0Au:
        case 0x0Bu: case 0x0Cu: case 0x0Du: {
            U32 w = (a & 0x01FFFFFFu) >> 2u;
            if (w < g_rom_words) return g_rom[w];
            return rom_open_bus(a);
        }
        default:
            return st.open_bus;
    }
}

// ---------------------------------------------------------------------------
// Loads. Each returns the value zero-extended into a U32; the caller applies
// any sign extension or unaligned rotation (that is CPU behaviour, not bus
// behaviour, so it lives in the instruction implementations).

CORE_FN U32 bus_read32(INOUT(GbaState) st, U32 addr) {
    if (((addr >> 24u) & 0x0Fu) >= 0x0Eu) return sram_read8(st, addr) * 0x01010101u;
    U32 w = bus_read_word_aligned(st, addr);
    st.open_bus = w;
    return w;
}

CORE_FN U32 bus_read16(INOUT(GbaState) st, U32 addr) {
    if (((addr >> 24u) & 0x0Fu) >= 0x0Eu) return sram_read8(st, addr) * 0x0101u;
    U32 w = bus_read_word_aligned(st, addr);
    st.open_bus = w;
    return (w >> ((addr & 2u) * 8u)) & 0xFFFFu;
}

CORE_FN U32 bus_read8(INOUT(GbaState) st, U32 addr) {
    if (((addr >> 24u) & 0x0Fu) >= 0x0Eu) return sram_read8(st, addr);
    U32 w = bus_read_word_aligned(st, addr);
    st.open_bus = w;
    return (w >> ((addr & 3u) * 8u)) & 0xFFu;
}

// ---------------------------------------------------------------------------
// Stores.

CORE_FN void bus_write32(INOUT(GbaState) st, U32 addr, U32 val) {
    U32 a = addr & 0xFFFFFFFCu;
    U32 region = (a >> 24u) & 0x0Fu;
    switch (region) {
        case 0x02u: g_ewram[MEM_IDX(EWRAM_WORDS, st.inst, (a & EWRAM_MASK) >> 2u)] = val; break;
        case 0x03u: g_iwram[MEM_IDX(IWRAM_WORDS, st.inst, (a & IWRAM_MASK) >> 2u)] = val; break;
        case 0x04u: {
            U32 off = a & 0x00FFFFFFu;
            if (off < IO_SIZE) g_io[MEM_IDX(IO_WORDS, st.inst, off >> 2u)] = val;
        } break;
        case 0x05u: g_pram[MEM_IDX(PRAM_WORDS, st.inst, (a & PRAM_MASK) >> 2u)] = val; break;
        case 0x06u: g_vram[MEM_IDX(VRAM_WORDS, st.inst, vram_offset(a) >> 2u)] = val; break;
        case 0x07u: g_oam[MEM_IDX(OAM_WORDS, st.inst, (a & OAM_MASK) >> 2u)] = val; break;
        case 0x0Eu: case 0x0Fu: sram_write8(st, addr, val); break;  // 8-bit bus
        default: break;  // BIOS and ROM are read-only
    }
}

CORE_FN void bus_write16(INOUT(GbaState) st, U32 addr, U32 val) {
    U32 a = addr & 0xFFFFFFFEu;
    U32 region = (a >> 24u) & 0x0Fu;
    U32 sh = (a & 2u) * 8u;           // 0 or 16
    U32 mask = 0xFFFFu << sh;
    U32 ins = (val & 0xFFFFu) << sh;
    U32 idx;
    switch (region) {
        case 0x02u:
            idx = MEM_IDX(EWRAM_WORDS, st.inst, (a & EWRAM_MASK) >> 2u);
            g_ewram[idx] = merge_field(g_ewram[idx], mask, ins);
            break;
        case 0x03u:
            idx = MEM_IDX(IWRAM_WORDS, st.inst, (a & IWRAM_MASK) >> 2u);
            g_iwram[idx] = merge_field(g_iwram[idx], mask, ins);
            break;
        case 0x04u: {
            U32 off = a & 0x00FFFFFFu;
            if (off < IO_SIZE) {
                idx = MEM_IDX(IO_WORDS, st.inst, off >> 2u);
                g_io[idx] = merge_field(g_io[idx], mask, ins);
            }
        } break;
        case 0x05u:
            idx = MEM_IDX(PRAM_WORDS, st.inst, (a & PRAM_MASK) >> 2u);
            g_pram[idx] = merge_field(g_pram[idx], mask, ins);
            break;
        case 0x06u:
            idx = MEM_IDX(VRAM_WORDS, st.inst, vram_offset(a) >> 2u);
            g_vram[idx] = merge_field(g_vram[idx], mask, ins);
            break;
        case 0x07u:
            idx = MEM_IDX(OAM_WORDS, st.inst, (a & OAM_MASK) >> 2u);
            g_oam[idx] = merge_field(g_oam[idx], mask, ins);
            break;
        case 0x0Eu: case 0x0Fu: sram_write8(st, addr, val); break;
        default: break;
    }
}

CORE_FN void bus_write8(INOUT(GbaState) st, U32 addr, U32 val) {
    U32 region = (addr >> 24u) & 0x0Fu;
    U32 b = val & 0xFFu;

    // Video-memory byte-write quirks (GBATEK). These are invisible to CPU test
    // ROMs and very visible in real games, so they belong here from the start:
    //   OAM      - 8-bit writes are dropped entirely
    //   PRAM     - become a halfword write of the byte doubled
    //   VRAM BG  - same doubling
    //   VRAM OBJ - dropped entirely
    if (region == 0x07u) return;
    if (region == 0x05u) { bus_write16(st, addr, b * 0x0101u); return; }
    if (region == 0x06u) {
        // OBJ VRAM begins at 0x10000 in the tiled modes and 0x14000 in the
        // bitmap modes, so the boundary depends on DISPCNT's mode field.
        U32 mode = g_io[MEM_IDX(IO_WORDS, st.inst, 0u)] & 0x07u;
        U32 obj_start = (mode >= 3u) ? 0x14000u : 0x10000u;
        if (vram_offset(addr) >= obj_start) return;
        bus_write16(st, addr, b * 0x0101u);
        return;
    }

    U32 sh = (addr & 3u) * 8u;
    U32 mask = 0xFFu << sh;
    U32 ins = b << sh;
    U32 idx;
    switch (region) {
        case 0x02u:
            idx = MEM_IDX(EWRAM_WORDS, st.inst, (addr & EWRAM_MASK) >> 2u);
            g_ewram[idx] = merge_field(g_ewram[idx], mask, ins);
            break;
        case 0x03u:
            idx = MEM_IDX(IWRAM_WORDS, st.inst, (addr & IWRAM_MASK) >> 2u);
            g_iwram[idx] = merge_field(g_iwram[idx], mask, ins);
            break;
        case 0x04u: {
            U32 off = addr & 0x00FFFFFFu;
            if (off < IO_SIZE) {
                idx = MEM_IDX(IO_WORDS, st.inst, off >> 2u);
                g_io[idx] = merge_field(g_io[idx], mask, ins);
            }
        } break;
        case 0x0Eu: case 0x0Fu: sram_write8(st, addr, b); break;
        default: break;
    }
}

#endif  // GBA_CORE_BUS_INC
