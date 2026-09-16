#ifndef GBA_CORE_PPU_INC
#define GBA_CORE_PPU_INC

#include "memmap.h"
#include "state.h"
#include "types.h"

// The picture processing unit.
//
// Rendering is per scanline: a line is drawn once, using the register state as
// of the end of that line. Real hardware latches per pixel, so mid-scanline
// register writes (raster effects) do not reproduce. That is the granularity
// the plan chose, not an oversight.
//
// The framebuffer holds native BGR555, two pixels per word, exactly as the GBA
// stores colour. Conversion to RGB888 happens on the host, at the one point
// where a picture is actually wanted.

// ---------------------------------------------------------------------------
// Raw video memory access.
//
// The PPU reads video memory directly rather than through the CPU bus: it is
// not subject to the CPU's byte-write quirks, and this skips the region decode
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

CORE_FN U32 oam_read16_raw(INOUT(GbaState) st, U32 off) {
    U32 w = g_oam[MEM_IDX(OAM_WORDS, st.inst, (off & OAM_MASK) >> 2u)];
    return (w >> ((off & 2u) * 8u)) & 0xFFFFu;
}

CORE_FN U32 io_read16_raw(INOUT(GbaState) st, U32 off) {
    U32 w = g_io[MEM_IDX(IO_WORDS, st.inst, (off & IO_MASK) >> 2u)];
    return (w >> ((off & 2u) * 8u)) & 0xFFFFu;
}

CORE_FN U32 io_read32_raw(INOUT(GbaState) st, U32 off) {
    return g_io[MEM_IDX(IO_WORDS, st.inst, (off & IO_MASK) >> 2u)];
}

CORE_FN void fb_write(INOUT(GbaState) st, U32 x, U32 y, U32 colour) {
    U32 pixel = y * SCREEN_W + x;
    U32 idx = MEM_IDX(FB_WORDS, st.inst, pixel >> 1u);
    U32 sh = (pixel & 1u) * 16u;
    U32 mask = 0xFFFFu << sh;
    g_fb[idx] = (g_fb[idx] & (~mask)) | ((colour & 0xFFFFu) << sh);
}

// ---------------------------------------------------------------------------
// Layer identifiers, used for priority resolution and as blend-target indices.
// The numbering matches the bit order of BLDCNT and WININ/WINOUT.

KCONST U32 LAYER_BG0 = 0u;
KCONST U32 LAYER_OBJ = 4u;
KCONST U32 LAYER_BACKDROP = 5u;
KCONST U32 LAYER_NONE = 6u;

// ---------------------------------------------------------------------------
// Text (regular) backgrounds.
//
// Returns a palette index, 0 meaning transparent.

CORE_FN U32 bg_text_pixel(INOUT(GbaState) st, U32 bg, U32 x, U32 y) {
    U32 cnt = io_read16_raw(st, 0x08u + bg * 2u);
    U32 hofs = io_read16_raw(st, 0x10u + bg * 4u) & 0x1FFu;
    U32 vofs = io_read16_raw(st, 0x12u + bg * 4u) & 0x1FFu;

    U32 char_base = ((cnt >> 2u) & 3u) * 0x4000u;
    U32 screen_base = ((cnt >> 8u) & 0x1Fu) * 0x800u;
    bool bpp8 = ((cnt >> 7u) & 1u) != 0u;
    U32 size = (cnt >> 14u) & 3u;

    // Screen sizes: 0 = 256x256, 1 = 512x256, 2 = 256x512, 3 = 512x512.
    U32 map_w = (size == 1u || size == 3u) ? 512u : 256u;
    U32 map_h = (size == 2u || size == 3u) ? 512u : 256u;
    U32 sx = (x + hofs) & (map_w - 1u);
    U32 sy = (y + vofs) & (map_h - 1u);

    // A map wider or taller than 256 is built from several 2 KiB screenblocks,
    // laid out left to right then top to bottom.
    U32 block = 0u;
    if (sx >= 256u) block = block + 1u;
    if (sy >= 256u) block = block + ((map_w == 512u) ? 2u : 1u);

    U32 tx = (sx & 255u) >> 3u;
    U32 ty = (sy & 255u) >> 3u;
    U32 entry = vram_read16_raw(st, screen_base + block * 0x800u + (ty * 32u + tx) * 2u);

    U32 tile = entry & 0x3FFu;
    U32 px = sx & 7u;
    U32 py = sy & 7u;
    if ((entry & 0x0400u) != 0u) px = 7u - px;  // horizontal flip
    if ((entry & 0x0800u) != 0u) py = 7u - py;  // vertical flip

    if (bpp8) {
        // 256 colours, one byte per pixel, 64 bytes per tile.
        return vram_read8_raw(st, char_base + tile * 64u + py * 8u + px);
    }
    // 16 colours, one nibble per pixel, 32 bytes per tile, palette from the map.
    U32 b = vram_read8_raw(st, char_base + tile * 32u + py * 4u + (px >> 1u));
    U32 idx = ((px & 1u) != 0u) ? (b >> 4u) : (b & 0x0Fu);
    if (idx == 0u) return 0u;
    return ((entry >> 12u) & 0x0Fu) * 16u + idx;
}

// ---------------------------------------------------------------------------
// Affine (rotation/scaling) backgrounds, BG2 and BG3.
//
// The reference point and matrix are applied directly rather than through the
// per-scanline internal registers hardware maintains. That is equivalent
// whenever the ROM does not rewrite them mid-frame, which matches the
// scanline granularity used everywhere else here.

CORE_FN U32 bg_affine_pixel(INOUT(GbaState) st, U32 bg, U32 x, U32 y) {
    U32 cnt = io_read16_raw(st, 0x08u + bg * 2u);
    U32 base = (bg == 2u) ? 0x20u : 0x30u;

    I32 pa = I32(sign_extend(io_read16_raw(st, base + 0u), 16u));
    I32 pb = I32(sign_extend(io_read16_raw(st, base + 2u), 16u));
    I32 pc = I32(sign_extend(io_read16_raw(st, base + 4u), 16u));
    I32 pd = I32(sign_extend(io_read16_raw(st, base + 6u), 16u));
    // Reference points are 28-bit signed with 8 fractional bits.
    I32 refx = I32(sign_extend(io_read32_raw(st, base + 8u), 28u));
    I32 refy = I32(sign_extend(io_read32_raw(st, base + 12u), 28u));

    // Sizes are 128, 256, 512 or 1024 pixels square.
    U32 shift = (cnt >> 14u) & 3u;
    U32 size = 128u << shift;
    U32 tiles = size >> 3u;

    I32 tex_x = (refx + pa * I32(x) + pb * I32(y)) >> 8;
    I32 tex_y = (refy + pc * I32(x) + pd * I32(y)) >> 8;

    if (U32(tex_x) >= size || U32(tex_y) >= size) {
        // Outside the map: either wrap around or show nothing, per BGxCNT.
        if ((cnt & 0x2000u) == 0u) return 0u;
        tex_x = I32(U32(tex_x) & (size - 1u));
        tex_y = I32(U32(tex_y) & (size - 1u));
    }

    U32 char_base = ((cnt >> 2u) & 3u) * 0x4000u;
    U32 screen_base = ((cnt >> 8u) & 0x1Fu) * 0x800u;
    U32 ux = U32(tex_x);
    U32 uy = U32(tex_y);

    // Affine maps use one byte per tile and are always 256 colours.
    U32 tile = vram_read8_raw(st, screen_base + (uy >> 3u) * tiles + (ux >> 3u));
    return vram_read8_raw(st, char_base + tile * 64u + (uy & 7u) * 8u + (ux & 7u));
}

// ---------------------------------------------------------------------------
// Sprites.
//
// Sprites are drawn into a scanline buffer once per line rather than searched
// per pixel: iterating 128 objects for each of 240 pixels would be 30,000
// tests a line. The buffer packs colour, priority and flags into one word.

KCONST U32 OBJ_PRESENT = 0x00010000u;
KCONST U32 OBJ_SEMI    = 0x00020000u;  // semi-transparent: forces alpha blending
KCONST U32 OBJ_WINDOW  = 0x00040000u;  // this object masks, it does not draw
// priority occupies bits 20-21

// Sprite dimensions from the shape and size fields.
CORE_FN U32 obj_width(U32 shape, U32 size) {
    if (shape == 0u) return 8u << size;                       // square
    if (shape == 1u) return (size == 0u) ? 16u : (size == 1u) ? 32u : (size == 2u) ? 32u : 64u;
    return (size == 0u) ? 8u : (size == 1u) ? 8u : (size == 2u) ? 16u : 32u;
}

CORE_FN U32 obj_height(U32 shape, U32 size) {
    if (shape == 0u) return 8u << size;
    if (shape == 1u) return (size == 0u) ? 8u : (size == 1u) ? 8u : (size == 2u) ? 16u : 32u;
    return (size == 0u) ? 16u : (size == 1u) ? 32u : (size == 2u) ? 32u : 64u;
}

// Fetches one texel of a sprite. Returns a palette index, 0 = transparent.
CORE_FN U32 obj_texel(INOUT(GbaState) st, U32 dispcnt, U32 attr0, U32 attr2, U32 w, U32 tx,
                      U32 ty) {
    bool bpp8 = ((attr0 >> 13u) & 1u) != 0u;
    U32 tile = attr2 & 0x3FFu;
    U32 tile_x = tx >> 3u;
    U32 tile_y = ty >> 3u;
    // Tile numbers count in 32-byte units, so a 256-colour tile takes two.
    U32 step = bpp8 ? 2u : 1u;

    // DISPCNT bit 6 selects one-dimensional mapping, where a sprite's tiles are
    // consecutive, over two-dimensional, where OBJ VRAM is a 32-tile-wide grid.
    U32 index;
    if ((dispcnt & 0x0040u) != 0u) index = tile + (tile_y * (w >> 3u) + tile_x) * step;
    else                           index = tile + tile_y * 32u + tile_x * step;

    // Sprite tiles live in the upper half of VRAM.
    U32 base = 0x10000u + index * 32u;
    if (bpp8) {
        U32 idx8 = vram_read8_raw(st, base + (ty & 7u) * 8u + (tx & 7u));
        return (idx8 == 0u) ? 0u : (256u + idx8);
    }
    U32 b = vram_read8_raw(st, base + (ty & 7u) * 4u + ((tx & 7u) >> 1u));
    U32 idx = ((tx & 1u) != 0u) ? (b >> 4u) : (b & 0x0Fu);
    if (idx == 0u) return 0u;
    return 256u + ((attr2 >> 12u) & 0x0Fu) * 16u + idx;  // OBJ palette is the second half
}


// ---------------------------------------------------------------------------
// One background layer's contribution at a pixel.
//
// Returns 0 for transparent, otherwise OPAQUE_BIT | colour. Unifying the tiled,
// affine and bitmap cases here lets the compositor treat every mode the same
// way -- which matches hardware, where the bitmap modes are simply BG2.

KCONST U32 OPAQUE_BIT = 0x10000u;

CORE_FN U32 bg_layer_pixel(INOUT(GbaState) st, U32 bg, U32 x, U32 y, U32 mode, U32 dispcnt) {
    // Mosaic quantises the sampling coordinates to a block grid.
    U32 cnt = io_read16_raw(st, 0x08u + bg * 2u);
    if ((cnt & 0x0040u) != 0u) {
        U32 mos = io_read16_raw(st, 0x4Cu);
        U32 mh = (mos & 0x0Fu) + 1u;
        U32 mv = ((mos >> 4u) & 0x0Fu) + 1u;
        x = (x / mh) * mh;
        y = (y / mv) * mv;
    }

    if (mode == 0u || (mode == 1u && bg < 2u)) {
        U32 idx = bg_text_pixel(st, bg, x, y);
        return (idx == 0u) ? 0u : (OPAQUE_BIT | pram_read16_raw(st, idx * 2u));
    }
    if ((mode == 1u && bg == 2u) || (mode == 2u && bg >= 2u)) {
        U32 idx = bg_affine_pixel(st, bg, x, y);
        return (idx == 0u) ? 0u : (OPAQUE_BIT | pram_read16_raw(st, idx * 2u));
    }
    if (mode >= 3u && bg == 2u) {
        // The bitmap modes are BG2, so they composite and blend like any layer.
        U32 frame = ((dispcnt & 0x0010u) != 0u) ? 0xA000u : 0x0000u;
        if (mode == 3u) return OPAQUE_BIT | vram_read16_raw(st, (y * SCREEN_W + x) * 2u);
        if (mode == 4u) {
            U32 idx = vram_read8_raw(st, frame + y * SCREEN_W + x);
            return (idx == 0u) ? 0u : (OPAQUE_BIT | pram_read16_raw(st, idx * 2u));
        }
        if (x < 160u && y < 128u)
            return OPAQUE_BIT | vram_read16_raw(st, frame + (y * 160u + x) * 2u);
        return 0u;
    }
    return 0u;  // this background does not exist in this mode
}

// ---------------------------------------------------------------------------
// Windows

CORE_FN bool in_window(INOUT(GbaState) st, U32 h_reg, U32 v_reg, U32 x, U32 y) {
    U32 h = io_read16_raw(st, h_reg);
    U32 v = io_read16_raw(st, v_reg);
    U32 x1 = (h >> 8u) & 0xFFu, x2 = h & 0xFFu;
    U32 y1 = (v >> 8u) & 0xFFu, y2 = v & 0xFFu;
    // A right or bottom edge that is less than the left or top edge wraps
    // around the screen rather than describing an empty region.
    bool ix = (x1 <= x2) ? (x >= x1 && x < x2) : (x >= x1 || x < x2);
    bool iy = (y1 <= y2) ? (y >= y1 && y < y2) : (y >= y1 || y < y2);
    return ix && iy;
}

// Bits 0-3 enable BG0-3, bit 4 the sprites, bit 5 the colour effects.
CORE_FN U32 window_mask(INOUT(GbaState) st, U32 dispcnt, U32 x, U32 y, bool in_obj_window) {
    bool w0 = (dispcnt & 0x2000u) != 0u;
    bool w1 = (dispcnt & 0x4000u) != 0u;
    bool wobj = (dispcnt & 0x8000u) != 0u;
    if (!w0 && !w1 && !wobj) return 0x3Fu;  // no windows active: everything shows

    U32 winin = io_read16_raw(st, 0x48u);
    U32 winout = io_read16_raw(st, 0x4Au);
    // Window 0 takes precedence over window 1, which takes precedence over the
    // object window; anything in none of them uses WINOUT.
    if (w0 && in_window(st, 0x40u, 0x44u, x, y)) return winin & 0x3Fu;
    if (w1 && in_window(st, 0x42u, 0x46u, x, y)) return (winin >> 8u) & 0x3Fu;
    if (wobj && in_obj_window) return (winout >> 8u) & 0x3Fu;
    return winout & 0x3Fu;
}

// ---------------------------------------------------------------------------
// Colour effects

CORE_FN U32 blend_alpha(U32 top, U32 bottom, U32 eva, U32 evb) {
    U32 r = umin(31u, ((top & 31u) * eva + (bottom & 31u) * evb) >> 4u);
    U32 g = umin(31u, (((top >> 5u) & 31u) * eva + ((bottom >> 5u) & 31u) * evb) >> 4u);
    U32 b = umin(31u, (((top >> 10u) & 31u) * eva + ((bottom >> 10u) & 31u) * evb) >> 4u);
    return r | (g << 5u) | (b << 10u);
}

CORE_FN U32 blend_brightness(U32 c, U32 evy, bool brighten) {
    U32 r = c & 31u, g = (c >> 5u) & 31u, b = (c >> 10u) & 31u;
    if (brighten) {
        r = r + (((31u - r) * evy) >> 4u);
        g = g + (((31u - g) * evy) >> 4u);
        b = b + (((31u - b) * evy) >> 4u);
    } else {
        r = r - ((r * evy) >> 4u);
        g = g - ((g * evy) >> 4u);
        b = b - ((b * evy) >> 4u);
    }
    return r | (g << 5u) | (b << 10u);
}

// ---------------------------------------------------------------------------

CORE_FN void ppu_render_scanline(INOUT(GbaState) st, U32 y) {
    U32 dispcnt = io_read16_raw(st, 0x00u);
    U32 mode = dispcnt & 7u;

    // Forced blank drives the display white regardless of anything else.
    if ((dispcnt & 0x0080u) != 0u) {
        NO_UNROLL for (U32 x = 0u; x < SCREEN_W; ++x) fb_write(st, x, y, 0x7FFFu);
        return;
    }

    // --- sprites into a scanline buffer ------------------------------------
    //
    // Built once per line by walking the 128 objects, rather than searched per
    // pixel: the latter would be 30,000 tests a line. Objects are walked from
    // last to first so that a lower-numbered object ends up on top, which is
    // the hardware's tie-break.
    U32 obj_line[240];
    NO_UNROLL for (U32 x = 0u; x < SCREEN_W; ++x) obj_line[x] = 0u;

    if ((dispcnt & 0x1000u) != 0u) {
        NO_UNROLL for (U32 n = 0u; n < 128u; ++n) {
            U32 i = 127u - n;
            U32 attr0 = oam_read16_raw(st, i * 8u + 0u);
            U32 attr1 = oam_read16_raw(st, i * 8u + 2u);
            U32 attr2 = oam_read16_raw(st, i * 8u + 4u);

            bool affine = ((attr0 >> 8u) & 1u) != 0u;
            bool double_size = ((attr0 >> 9u) & 1u) != 0u;
            // Bit 9 means "double the drawing box" for an affine object, but
            // "disabled" for a normal one.
            if (!affine && double_size) continue;

            U32 shape = (attr0 >> 14u) & 3u;
            U32 size = (attr1 >> 14u) & 3u;
            U32 w = obj_width(shape, size);
            U32 h = obj_height(shape, size);
            U32 box_w = (affine && double_size) ? (w * 2u) : w;
            U32 box_h = (affine && double_size) ? (h * 2u) : h;

            // Y is an 8-bit wrapping coordinate, so a sprite can straddle the
            // top of the screen by being placed near 255.
            U32 oy = attr0 & 0xFFu;
            U32 line = (y - oy) & 0xFFu;
            if (line >= box_h) continue;

            U32 mode_bits = (attr0 >> 10u) & 3u;
            if (mode_bits == 3u) continue;  // invalid
            bool is_window = (mode_bits == 2u);
            U32 prio = (attr2 >> 10u) & 3u;

            I32 ox = I32(sign_extend(attr1 & 0x1FFu, 9u));  // X is signed 9-bit

            I32 pa = 256, pb = 0, pc = 0, pd = 256;  // identity, 8 fractional bits
            if (affine) {
                U32 m = (attr1 >> 9u) & 0x1Fu;
                pa = I32(sign_extend(oam_read16_raw(st, m * 32u + 0x06u), 16u));
                pb = I32(sign_extend(oam_read16_raw(st, m * 32u + 0x0Eu), 16u));
                pc = I32(sign_extend(oam_read16_raw(st, m * 32u + 0x16u), 16u));
                pd = I32(sign_extend(oam_read16_raw(st, m * 32u + 0x1Eu), 16u));
            }

            NO_UNROLL for (U32 px = 0u; px < box_w; ++px) {
                I32 sx = ox + I32(px);
                if (sx < 0 || sx >= I32(SCREEN_W)) continue;

                U32 tx, ty;
                if (affine) {
                    // Rotate about the centre of the drawing box.
                    I32 dx = I32(px) - I32(box_w >> 1u);
                    I32 dy = I32(line) - I32(box_h >> 1u);
                    I32 rx = ((pa * dx + pb * dy) >> 8) + I32(w >> 1u);
                    I32 ry = ((pc * dx + pd * dy) >> 8) + I32(h >> 1u);
                    if (rx < 0 || ry < 0 || rx >= I32(w) || ry >= I32(h)) continue;
                    tx = U32(rx);
                    ty = U32(ry);
                } else {
                    tx = ((attr1 & 0x1000u) != 0u) ? (w - 1u - px) : px;    // horizontal flip
                    ty = ((attr1 & 0x2000u) != 0u) ? (h - 1u - line) : line;  // vertical flip
                }

                U32 idx = obj_texel(st, dispcnt, attr0, attr2, w, tx, ty);
                if (idx == 0u) continue;

                if (is_window) {
                    // An object-window sprite contributes no colour; it only
                    // marks the pixels its opaque texels cover.
                    obj_line[U32(sx)] = obj_line[U32(sx)] | OBJ_WINDOW;
                    continue;
                }
                U32 obj_pixel = pram_read16_raw(st, idx * 2u) | OBJ_PRESENT | (prio << 20u);
                if (mode_bits == 1u) obj_pixel = obj_pixel | OBJ_SEMI;
                // Preserve any window mark already recorded at this pixel.
                obj_line[U32(sx)] = obj_pixel | (obj_line[U32(sx)] & OBJ_WINDOW);
            }
        }
    }

    // --- per-pixel compositing ---------------------------------------------
    U32 backdrop = pram_read16_raw(st, 0u);
    U32 bldcnt = io_read16_raw(st, 0x50u);
    U32 effect = (bldcnt >> 6u) & 3u;
    U32 bldalpha = io_read16_raw(st, 0x52u);
    U32 eva = umin(16u, bldalpha & 0x1Fu);
    U32 evb = umin(16u, (bldalpha >> 8u) & 0x1Fu);
    U32 evy = umin(16u, io_read16_raw(st, 0x54u) & 0x1Fu);

    NO_UNROLL for (U32 x = 0u; x < SCREEN_W; ++x) {
        U32 obj = obj_line[x];
        U32 mask = window_mask(st, dispcnt, x, y, (obj & OBJ_WINDOW) != 0u);

        // Evaluate each background once, then resolve priority over the cached
        // results. Re-fetching per priority pass would cost up to four times as
        // many tile lookups.
        U32 bg_pixel[4];
        U32 bg_prio[4];
        NO_UNROLL for (U32 b = 0u; b < 4u; ++b) {
            bg_pixel[b] = 0u;
            bg_prio[b] = 4u;
            if ((dispcnt & (0x0100u << b)) == 0u) continue;  // layer disabled
            if ((mask & (1u << b)) == 0u) continue;          // hidden by a window
            bg_pixel[b] = bg_layer_pixel(st, b, x, y, mode, dispcnt);
            bg_prio[b] = io_read16_raw(st, 0x08u + b * 2u) & 3u;
        }

        bool obj_visible = (obj & OBJ_PRESENT) != 0u && (mask & 0x10u) != 0u;
        U32 obj_prio = (obj >> 20u) & 3u;

        // Walk priorities from front to back, taking the top two layers: the
        // second is needed as the lower operand for alpha blending.
        U32 top_colour = backdrop, top_layer = LAYER_BACKDROP;
        U32 second_colour = backdrop, second_layer = LAYER_BACKDROP;
        U32 found = 0u;
        for (U32 p = 0u; p < 4u && found < 2u; ++p) {
            // A sprite beats a background of the same priority.
            if (obj_visible && obj_prio == p) {
                if (found == 0u) { top_colour = obj & 0xFFFFu; top_layer = LAYER_OBJ; }
                else if (found == 1u) { second_colour = obj & 0xFFFFu; second_layer = LAYER_OBJ; }
                ++found;
            }
            for (U32 b = 0u; b < 4u && found < 2u; ++b) {
                if (bg_prio[b] != p || (bg_pixel[b] & OPAQUE_BIT) == 0u) continue;
                if (found == 0u) { top_colour = bg_pixel[b] & 0xFFFFu; top_layer = LAYER_BG0 + b; }
                else { second_colour = bg_pixel[b] & 0xFFFFu; second_layer = LAYER_BG0 + b; }
                ++found;
            }
        }

        U32 colour = top_colour;
        bool fx_allowed = (mask & 0x20u) != 0u;
        bool target1 = (bldcnt & (1u << top_layer)) != 0u;
        bool target2 = ((bldcnt >> 8u) & (1u << second_layer)) != 0u;
        bool semi = (top_layer == LAYER_OBJ) && ((obj & OBJ_SEMI) != 0u);

        if (fx_allowed && semi && target2) {
            // A semi-transparent sprite alpha-blends whatever BLDCNT selects.
            colour = blend_alpha(top_colour, second_colour, eva, evb);
        } else if (fx_allowed && target1 && effect == 1u && target2) {
            colour = blend_alpha(top_colour, second_colour, eva, evb);
        } else if (fx_allowed && target1 && effect == 2u) {
            colour = blend_brightness(top_colour, evy, true);
        } else if (fx_allowed && target1 && effect == 3u) {
            colour = blend_brightness(top_colour, evy, false);
        }

        fb_write(st, x, y, colour);
    }
}


// ---------------------------------------------------------------------------
// Observations.
//
// The framebuffer reduced 4x and to 8-bit grayscale. A throughput workload
// wants a small, consistent tensor rather than a picture, and at 4096 instances
// the difference is 9.6 MiB a frame against 300 MiB.
//
// Nearest sampling from the centre of each 4x4 block, not an average: the
// average costs sixteen reads per output pixel and RL agents are far more
// sensitive to consistency than to resampling quality.
CORE_FN void ppu_write_observation(INOUT(GbaState) st) {
    NO_UNROLL for (U32 oy = 0u; oy < OBS_H; ++oy) {
        NO_UNROLL for (U32 ox = 0u; ox < OBS_W; ++ox) {
            U32 pixel = (oy * 4u + 2u) * SCREEN_W + (ox * 4u + 2u);
            U32 colour =
                (g_fb[MEM_IDX(FB_WORDS, st.inst, pixel >> 1u)] >> ((pixel & 1u) * 16u)) & 0xFFFFu;

            // BGR555 to luminance with the usual 601 weights. Each channel is
            // 0-31 and the weights sum to 256, so the result is 0-31 again;
            // replicating the top bits scales it to a full 0-255.
            U32 r = colour & 31u;
            U32 g = (colour >> 5u) & 31u;
            U32 b = (colour >> 10u) & 31u;
            U32 lum = (r * 77u + g * 150u + b * 29u) >> 8u;
            lum = (lum << 3u) | (lum >> 2u);

            U32 idx = oy * OBS_W + ox;
            U32 w = OBS_IDX(st.inst, idx >> 2u);
            U32 sh = (idx & 3u) * 8u;
            g_obs[w] = (g_obs[w] & (~(0xFFu << sh))) | (lum << sh);
        }
    }
}

#endif  // GBA_CORE_PPU_INC
