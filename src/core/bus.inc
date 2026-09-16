#ifndef GBA_CORE_BUS_INC
#define GBA_CORE_BUS_INC

#include "io.inc"
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
// Save memory.
//
// Modelled as 128 KiB Flash, which is what most later cartridges use and what
// Pokemon Emerald expects. Flash is not a RAM array: writes to two magic
// addresses unlock each operation, and a game identifies the chip by putting it
// into ID mode and reading two bytes. A game that cannot identify the chip
// concludes the cartridge has no save memory -- Emerald responds by setting its
// main callback to NULL, so it runs forever and draws nothing.
//
// The device presented is a Macronix 128 KiB part, the usual choice for this
// size. Banks are 64 KiB; the upper one is selected by a bank-switch command.

KCONST U32 FLASH_MANUFACTURER = 0xC2u;  // Macronix
KCONST U32 FLASH_DEVICE = 0x09u;        // MX29L010, 128 KiB
KCONST U32 FLASH_CMD_ADDR1 = 0x5555u;
KCONST U32 FLASH_CMD_ADDR2 = 0x2AAAu;
KCONST U32 FLASH_BANK_SIZE = 0x10000u;

CORE_FN U32 sram_read8(INOUT(GbaState) st, U32 addr) {
    U32 off = addr & 0xFFFFu;

    // While in ID mode the first two bytes read as the chip identity.
    if (st.flash_id_mode != 0u) {
        if (off == 0u) return FLASH_MANUFACTURER;
        if (off == 1u) return FLASH_DEVICE;
    }

    U32 byte_index = st.flash_bank * FLASH_BANK_SIZE + off;
    U32 w = g_sram[MEM_IDX(SRAM_WORDS, st.inst, byte_index >> 2u)];
    return (w >> ((byte_index & 3u) * 8u)) & 0xFFu;
}

CORE_FN void flash_store(INOUT(GbaState) st, U32 off, U32 val) {
    U32 byte_index = st.flash_bank * FLASH_BANK_SIZE + off;
    U32 idx = MEM_IDX(SRAM_WORDS, st.inst, byte_index >> 2u);
    U32 sh = (byte_index & 3u) * 8u;
    g_sram[idx] = merge_field(g_sram[idx], 0xFFu << sh, (val & 0xFFu) << sh);
}

// Erases a region back to 0xFF. Flash can only clear bits by erasing.
CORE_FN void flash_erase_range(INOUT(GbaState) st, U32 first_byte, U32 length) {
    for (U32 i = 0u; i < length; i = i + 4u) {
        U32 b = first_byte + i;
        g_sram[MEM_IDX(SRAM_WORDS, st.inst, b >> 2u)] = 0xFFFFFFFFu;
    }
}

CORE_FN void sram_write8(INOUT(GbaState) st, U32 addr, U32 val) {
    U32 off = addr & 0xFFFFu;
    U32 b = val & 0xFFu;

    // A program or bank-select command consumes the single write that follows
    // it, wherever it is addressed.
    if (st.flash_phase == 6u) {  // byte program
        flash_store(st, off, b);
        st.flash_phase = 0u;
        return;
    }
    if (st.flash_phase == 7u) {  // bank select
        st.flash_bank = b & 1u;
        st.flash_phase = 0u;
        return;
    }

    // The unlock sequence: 0xAA to 0x5555, then 0x55 to 0x2AAA, then a command.
    // An erase repeats the whole sequence, with the erase already armed by a
    // preceding 0x80 -- so the second pass simply returns to the command phase
    // rather than needing states of its own.
    if (st.flash_phase == 0u) {
        if (off == FLASH_CMD_ADDR1 && b == 0xAAu) st.flash_phase = 1u;
        return;
    }
    if (st.flash_phase == 1u) {
        st.flash_phase = (off == FLASH_CMD_ADDR2 && b == 0x55u) ? 2u : 0u;
        return;
    }

    // Phase 2: the command byte.
    st.flash_phase = 0u;

    // Sector erase names its sector by the address it is written to, so it is
    // the one command not issued at 0x5555.
    if (b == 0x30u && st.flash_erase != 0u) {
        st.flash_erase = 0u;
        flash_erase_range(st, st.flash_bank * FLASH_BANK_SIZE + (off & 0xF000u), 0x1000u);
        return;
    }
    if (off != FLASH_CMD_ADDR1) return;

    if (b == 0x90u) { st.flash_id_mode = 1u; return; }  // enter ID mode
    if (b == 0xF0u) { st.flash_id_mode = 0u; return; }  // leave ID mode
    if (b == 0x80u) { st.flash_erase = 1u; return; }    // arm an erase
    if (b == 0xA0u) { st.flash_phase = 6u; return; }    // program one byte
    if (b == 0xB0u) { st.flash_phase = 7u; return; }    // select bank
    if (b == 0x10u && st.flash_erase != 0u) {           // erase the whole chip
        st.flash_erase = 0u;
        flash_erase_range(st, 0u, SRAM_SIZE);
        return;
    }
}

// ---------------------------------------------------------------------------
// I/O registers whose write does something beyond storing a value.
//
// Nothing here performs a transfer: a DMA triggered from inside a store would
// be re-entrant, so the channel is only latched and the scheduler runs it.

CORE_FN void dma_latch(INOUT(GbaState) st, U32 ch) {
    U32 base = REG_DMA0SAD + ch * 12u;
    // Channel 0 cannot read the cartridge; channel 3 is the only one that can
    // write to it. The address masks encode that.
    st.dma_src[ch] = io_get32(st, base) & ((ch == 0u) ? 0x07FFFFFFu : 0x0FFFFFFFu);
    st.dma_dst[ch] = io_get32(st, base + 4u) & ((ch == 3u) ? 0x0FFFFFFFu : 0x07FFFFFFu);
    U32 count = io_get16(st, base + 8u);
    U32 limit = (ch == 3u) ? 0x10000u : 0x4000u;
    st.dma_count[ch] = (count == 0u) ? limit : count;
    st.dma_enabled = st.dma_enabled | (1u << ch);
}

// Filters a register write. Returns true when the plain store must be
// suppressed, having already applied whatever the register does instead.
CORE_FN bool io_store_filtered(INOUT(GbaState) st, U32 off, U32 value) {
    // Writing a 1 to a bit of IF acknowledges that interrupt rather than
    // raising it; storing the value directly would make every acknowledgement
    // re-assert the flag it was trying to clear.
    if (off == REG_IF) {
        io_set16(st, REG_IF, io_get16(st, REG_IF) & (~(value & 0xFFFFu)));
        return true;
    }
    // The low three bits of DISPSTAT are status driven by the PPU.
    if (off == REG_DISPSTAT) {
        io_set16(st, REG_DISPSTAT, (io_get16(st, REG_DISPSTAT) & 0x07u) | (value & 0xFFF8u));
        return true;
    }
    if (off == REG_VCOUNT) return true;    // read-only
    if (off == REG_KEYINPUT) return true;  // driven by the host

    // A timer's low register reads as the live counter but writes the reload.
    for (U32 t = 0u; t < 4u; ++t) {
        if (off == REG_TM0CNT_L + t * 4u) {
            st.timer_reload[t] = value & 0xFFFFu;
            return true;
        }
    }
    return false;
}

// Runs a register's side effect AFTER the store has landed, given what the
// register held beforehand so a 0->1 edge can be detected.
//
// Separating this from the store is not tidiness. A 32-bit write to DMAxCNT
// sets the count and the control word at once; latching the channel from
// inside the control half's handler would read a count that had not been
// stored yet. Pokemon Emerald's DMA3 copy does exactly this, and a stale count
// of zero means "maximum", so 512 words became 65536 and flattened the stack.
CORE_FN void io_after_store(INOUT(GbaState) st, U32 off, U32 old_value) {
    for (U32 ch = 0u; ch < 4u; ++ch) {
        if (off == REG_DMA0SAD + ch * 12u + 10u) {
            if ((io_get16(st, off) & 0x8000u) != 0u && (old_value & 0x8000u) == 0u)
                dma_latch(st, ch);
            return;
        }
    }
    for (U32 t = 0u; t < 4u; ++t) {
        if (off == REG_TM0CNT_L + t * 4u + 2u) {
            // Enabling a timer reloads its counter; leaving it enabled does not.
            if ((io_get16(st, off) & 0x0080u) != 0u && (old_value & 0x0080u) == 0u) {
                st.timer_counter[t] = st.timer_reload[t];
                st.timer_prescale[t] = 0u;
            }
            return;
        }
    }
}

CORE_FN void io_write_half(INOUT(GbaState) st, U32 off, U32 value) {
    U32 old = io_get16(st, off);
    if (!io_store_filtered(st, off, value)) io_set16(st, off, value);
    io_after_store(st, off, old);
}

CORE_FN void io_write_word(INOUT(GbaState) st, U32 off, U32 value) {
    U32 old_lo = io_get16(st, off);
    U32 old_hi = io_get16(st, off + 2u);
    // Both halves land before either side effect runs.
    if (!io_store_filtered(st, off, value & 0xFFFFu)) io_set16(st, off, value & 0xFFFFu);
    if (!io_store_filtered(st, off + 2u, value >> 16u)) io_set16(st, off + 2u, value >> 16u);
    io_after_store(st, off, old_lo);
    io_after_store(st, off + 2u, old_hi);
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
            if (off < IO_SIZE) io_write_word(st, off, val);
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
            if (off < IO_SIZE) io_write_half(st, off, val & 0xFFFFu);
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
                // Rebuild the containing halfword so a byte write to a register
                // with side effects is seen as the register write it is.
                U32 half_addr = off & (~1u);
                U32 cur = io_get16(st, half_addr);
                U32 merged = ((off & 1u) != 0u) ? ((cur & 0x00FFu) | (b << 8u))
                                                : ((cur & 0xFF00u) | b);
                io_write_half(st, half_addr, merged);
            }
        } break;
        case 0x0Eu: case 0x0Fu: sram_write8(st, addr, b); break;
        default: break;
    }
}

#endif  // GBA_CORE_BUS_INC
