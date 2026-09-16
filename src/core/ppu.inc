#ifndef GBA_CORE_PPU_INC
#define GBA_CORE_PPU_INC

#include "memmap.h"
#include "state.h"
#include "types.h"

// The picture processing unit.
//
// M5 covers the three bitmap modes only; the tiled modes and sprites arrive in
// M6. Rendering is per scanline, at the granularity the plan settled on: a line
// is drawn once, using the register state as of the end of that line. Real
// hardware latches per pixel, so mid-scanline register writes (a raster effect)
// will not reproduce. That is a deliberate scope decision, not an oversight.
//
// The framebuffer holds native BGR555, two pixels per word, exactly as the GBA
// stores colour. Conversion to RGB888 happens on the host, at the one point
// where a picture is actually wanted.

// The PPU reads video memory directly rather than through the CPU bus: it is
// not subject to the CPU's byte-write quirks, and this avoids the region decode
// on every pixel.
CORE_FN U32 vram_read8_raw(INOUT(GbaState) st, U32 off) {
    U32 w = g_vram[MEM_IDX(VRAM_WORDS, st.inst, off >> 2u)];
    return (w >> ((off & 3u) * 8u)) & 0xFFu;
}

CORE_FN U32 vram_read16_raw(INOUT(GbaState) st, U32 off) {
    U32 w = g_vram[MEM_IDX(VRAM_WORDS, st.inst, off >> 2u)];
    return (w >> ((off & 2u) * 8u)) & 0xFFFFu;
}

CORE_FN U32 pram_read16_raw(INOUT(GbaState) st, U32 off) {
    U32 w = g_pram[MEM_IDX(PRAM_WORDS, st.inst, (off & PRAM_MASK) >> 2u)];
    return (w >> ((off & 2u) * 8u)) & 0xFFFFu;
}

CORE_FN U32 io_read16_raw(INOUT(GbaState) st, U32 off) {
    U32 w = g_io[MEM_IDX(IO_WORDS, st.inst, (off & IO_MASK) >> 2u)];
    return (w >> ((off & 2u) * 8u)) & 0xFFFFu;
}

CORE_FN void fb_write(INOUT(GbaState) st, U32 x, U32 y, U32 colour) {
    U32 pixel = y * SCREEN_W + x;
    U32 idx = MEM_IDX(FB_WORDS, st.inst, pixel >> 1u);
    U32 sh = (pixel & 1u) * 16u;
    U32 mask = 0xFFFFu << sh;
    g_fb[idx] = (g_fb[idx] & (~mask)) | ((colour & 0xFFFFu) << sh);
}

// Palette entry 0 shows wherever nothing else is drawn.
CORE_FN U32 ppu_backdrop(INOUT(GbaState) st) { return pram_read16_raw(st, 0u); }

CORE_FN void ppu_render_scanline(INOUT(GbaState) st, U32 y) {
    U32 dispcnt = io_read16_raw(st, 0x00u);
    U32 mode = dispcnt & 7u;

    // Forced blank drives the display white regardless of any other state.
    if ((dispcnt & 0x0080u) != 0u) {
        for (U32 x = 0u; x < SCREEN_W; ++x) fb_write(st, x, y, 0x7FFFu);
        return;
    }

    // Modes 4 and 5 are double-buffered; DISPCNT bit 4 picks the visible frame.
    U32 frame_base = ((dispcnt & 0x0010u) != 0u) ? 0xA000u : 0x0000u;
    U32 backdrop = ppu_backdrop(st);

    if (mode == 3u) {
        // 240x160, one 16-bit colour per pixel, no palette, no second frame.
        U32 row = y * SCREEN_W * 2u;
        for (U32 x = 0u; x < SCREEN_W; ++x) fb_write(st, x, y, vram_read16_raw(st, row + x * 2u));
        return;
    }
    if (mode == 4u) {
        // 240x160, one palette index per pixel.
        U32 row = frame_base + y * SCREEN_W;
        for (U32 x = 0u; x < SCREEN_W; ++x) {
            U32 index = vram_read8_raw(st, row + x);
            fb_write(st, x, y, (index == 0u) ? backdrop : pram_read16_raw(st, index * 2u));
        }
        return;
    }
    if (mode == 5u) {
        // 160x128 of 16-bit colour, backdrop everywhere outside it.
        for (U32 x = 0u; x < SCREEN_W; ++x) {
            if (x < 160u && y < 128u) {
                fb_write(st, x, y, vram_read16_raw(st, frame_base + (y * 160u + x) * 2u));
            } else {
                fb_write(st, x, y, backdrop);
            }
        }
        return;
    }

    // Tiled modes 0-2 arrive in M6. Filling with the backdrop makes an
    // unimplemented mode obvious rather than leaving stale pixels on screen.
    for (U32 x = 0u; x < SCREEN_W; ++x) fb_write(st, x, y, backdrop);
}

#endif  // GBA_CORE_PPU_INC
