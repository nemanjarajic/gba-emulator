// Gate for the PPU features the earlier milestones implemented but never
// tested: 8bpp background tiles, background maps larger than one screenblock,
// two-dimensional sprite tile mapping, every sprite shape and size, mosaic,
// the object window, and semi-transparent sprites.
//
// Each was written from the hardware documentation and then left unexercised,
// which is exactly where bugs survive.

#include "app/ppu_scene.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace gba;
using namespace gba::scene;

namespace {

constexpr uint32_t kObjTileBase = 0x10000u;
constexpr uint32_t kObjPalette = 0x200u;  // the second half of palette RAM

// Distinct, unmistakable colours for the sprite quadrant tests.
const uint32_t kQuad[5] = {0x0000, 0x001F, 0x03E0, 0x7C00, 0x7FFF};

// Fills a 4bpp tile with a single palette index.
void fillTile4bpp(Scene& s, uint32_t byteOffset, uint32_t index) {
    const uint32_t packed = index * 0x11111111u;
    for (uint32_t w = 0; w < 8; ++w) s.vram[byteOffset / 4 + w] = packed;
}

// A 256-colour tile whose pixel (px, py) holds index py*8 + px, so every pixel
// of the tile is distinguishable and a mis-addressed row or column shows up.
void fillTile8bppRamp(Scene& s, uint32_t byteOffset) {
    for (uint32_t py = 0; py < 8; ++py)
        for (uint32_t px = 0; px < 8; ++px)
            s.vram[(byteOffset + py * 8 + px) / 4] |= (py * 8 + px) << (((py * 8 + px) & 3) * 8);
}

}  // namespace

int main() {
    VkContext ctx;
    ctx.init(/*validation=*/false, /*debugPrintf=*/false);

    // --- 8bpp background tiles ---------------------------------------------
    std::printf("8bpp background tiles\n");
    {
        Scene s;
        disableAllObjects(s);
        // Palette entry i is colour i, so a rendered pixel reports the palette
        // index the tile fetch produced.
        for (uint32_t i = 1; i < 64; ++i) poke16(s.pram, i * 2, i);
        fillTile8bppRamp(s, kTileBase);
        for (uint32_t i = 0; i < 32 * 32; ++i) poke16(s.vram, kBg0Map + i * 2, 0);

        poke16(s.io, 0x00, 0x0100);                        // mode 0, BG0
        poke16(s.io, 0x08, (28u << 8) | (1u << 7));        // BG0CNT: 256 colours
        Result r = runScene(ctx, s);

        bool ok = true;
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < 8; ++x) {
                const uint32_t want = y * 8 + x;      // index 0 is transparent
                const uint32_t got = pixelAt(r.cpuFb, x, y);
                if (got != (want == 0 ? 0u : want)) {
                    if (ok) std::printf("    (%u,%u): got %04X want %04X\n", x, y, got, want);
                    ok = false;
                }
            }
        check(ok, "every pixel of a 256-colour tile decodes correctly");
        checkParity(r, "8bpp scene matches on the GPU");
    }

    // --- background maps bigger than one screenblock ------------------------
    std::printf("large background maps\n");
    {
        // Screen sizes 1, 2 and 3 are built from 2, 2 and 4 screenblocks laid
        // out left to right then top to bottom. Each block gets its own tile,
        // and scrolling selects between them.
        struct Case {
            const char* name;
            uint32_t size;      // BGxCNT bits 14-15
            uint32_t hofs, vofs;
            uint32_t expectBlock;
        };
        const Case cases[] = {
            {"512x256, no scroll",   1, 0,   0,   0},
            {"512x256, scrolled right", 1, 256, 0,   1},
            {"256x512, scrolled down",  2, 0,   256, 1},
            {"512x512, scrolled both",  3, 256, 256, 3},
            {"512x512, scrolled right", 3, 256, 0,   1},
            {"512x512, scrolled down",  3, 0,   256, 2},
        };
        for (const Case& c : cases) {
            Scene s;
            disableAllObjects(s);
            for (uint32_t b = 0; b < 4; ++b) {
                poke16(s.pram, (b + 1) * 2, kQuad[b + 1]);
                fillTile4bpp(s, kTileBase + b * 32, b + 1);
                // Screenblock b starts at screen base + b * 0x800.
                for (uint32_t i = 0; i < 32 * 32; ++i)
                    poke16(s.vram, 28u * 0x800 + b * 0x800 + i * 2, b);
            }
            poke16(s.io, 0x00, 0x0100);
            poke16(s.io, 0x08, (28u << 8) | (c.size << 14));
            poke16(s.io, 0x10, c.hofs);
            poke16(s.io, 0x12, c.vofs);

            Result r = runScene(ctx, s);
            const uint32_t got = pixelAt(r.cpuFb, 4, 4);
            const uint32_t want = kQuad[c.expectBlock + 1];
            if (got != want) std::printf("    got %04X want %04X\n", got, want);
            check(got == want, (std::string("map ") + c.name).c_str());
            checkParity(r, "  ...and matches on the GPU");
        }
    }

    // --- two-dimensional sprite tile mapping --------------------------------
    std::printf("sprite tile mapping\n");
    for (int oneDimensional = 1; oneDimensional >= 0; --oneDimensional) {
        Scene s;
        disableAllObjects(s);
        for (uint32_t i = 1; i <= 4; ++i) poke16(s.pram, kObjPalette + i * 2, kQuad[i]);

        // A 16x16 sprite is 2x2 tiles. One-dimensional mapping numbers them
        // consecutively; two-dimensional treats OBJ VRAM as 32 tiles wide.
        const uint32_t tiles1d[4] = {0, 1, 2, 3};
        const uint32_t tiles2d[4] = {0, 1, 32, 33};
        const uint32_t* tiles = oneDimensional ? tiles1d : tiles2d;
        for (uint32_t q = 0; q < 4; ++q)
            fillTile4bpp(s, kObjTileBase + tiles[q] * 32, q + 1);

        poke16(s.io, 0x00, 0x1000u | (oneDimensional ? 0x40u : 0u));
        poke16(s.oam, 0, 40u);                     // Y = 40, square, 4bpp
        poke16(s.oam, 2, 40u | (1u << 14));        // X = 40, size 1 => 16x16
        poke16(s.oam, 4, 0);                       // tile 0

        Result r = runScene(ctx, s);
        const uint32_t sx[4] = {44, 52, 44, 52};
        const uint32_t sy[4] = {44, 44, 52, 52};
        bool ok = true;
        for (int q = 0; q < 4; ++q)
            if (pixelAt(r.cpuFb, sx[q], sy[q]) != kQuad[q + 1]) ok = false;
        check(ok, oneDimensional ? "one-dimensional tile mapping"
                                 : "two-dimensional tile mapping");
        checkParity(r, "  ...and matches on the GPU");
    }

    // --- every sprite shape and size ---------------------------------------
    std::printf("sprite shapes and sizes\n");
    {
        struct Dim { uint32_t shape, size, w, h; };
        const Dim dims[] = {
            {0, 0, 8, 8},   {0, 1, 16, 16}, {0, 2, 32, 32}, {0, 3, 64, 64},
            {1, 0, 16, 8},  {1, 1, 32, 8},  {1, 2, 32, 16}, {1, 3, 64, 32},
            {2, 0, 8, 16},  {2, 1, 8, 32},  {2, 2, 16, 32}, {2, 3, 32, 64},
        };
        bool allOk = true;
        for (const Dim& d : dims) {
            Scene s;
            disableAllObjects(s);
            poke16(s.pram, kObjPalette + 2, kQuad[1]);
            // A 64x64 sprite needs 64 tiles; fill enough for any size.
            for (uint32_t t = 0; t < 128; ++t) fillTile4bpp(s, kObjTileBase + t * 32, 1);

            poke16(s.io, 0x00, 0x1040u);                    // OBJ on, 1D mapping
            poke16(s.oam, 0, 20u | (d.shape << 14));        // Y = 20
            poke16(s.oam, 2, 20u | (d.size << 14));         // X = 20
            poke16(s.oam, 4, 0);

            Result r = runScene(ctx, s);
            const bool inside = pixelAt(r.cpuFb, 20 + d.w - 1, 20 + d.h - 1) == kQuad[1];
            const bool rightEdge = pixelAt(r.cpuFb, 20 + d.w, 20) == 0;
            const bool bottomEdge = pixelAt(r.cpuFb, 20, 20 + d.h) == 0;
            if (!(inside && rightEdge && bottomEdge)) {
                std::printf("    shape %u size %u (%ux%u): inside=%d right=%d bottom=%d\n",
                            d.shape, d.size, d.w, d.h, inside, rightEdge, bottomEdge);
                allOk = false;
            }
            if (r.cpuFb != r.gpuFb) allOk = false;
        }
        check(allOk, "all twelve shape/size combinations have the right extent");
    }

    // --- mosaic -------------------------------------------------------------
    std::printf("mosaic\n");
    {
        Scene s;
        disableAllObjects(s);
        for (uint32_t i = 1; i < 64; ++i) poke16(s.pram, i * 2, i);
        fillTile8bppRamp(s, kTileBase);
        for (uint32_t i = 0; i < 32 * 32; ++i) poke16(s.vram, kBg0Map + i * 2, 0);

        poke16(s.io, 0x00, 0x0100);
        poke16(s.io, 0x08, (28u << 8) | (1u << 7) | (1u << 6));  // 256 colours, mosaic
        poke16(s.io, 0x4C, 3u | (3u << 4));                      // 4x4 mosaic blocks
        Result r = runScene(ctx, s);

        // Sampling is quantised to the block origin, so the first four columns
        // all show what column 0 shows.
        const uint32_t origin = pixelAt(r.cpuFb, 0, 0);
        bool ok = true;
        for (uint32_t x = 1; x < 4; ++x)
            if (pixelAt(r.cpuFb, x, 0) != origin) ok = false;
        for (uint32_t y = 1; y < 4; ++y)
            if (pixelAt(r.cpuFb, 0, y) != origin) ok = false;
        check(ok, "a 4x4 mosaic block is uniform");
        check(pixelAt(r.cpuFb, 4, 0) != origin, "the next block differs");
        check(pixelAt(r.cpuFb, 0, 4) != origin, "the next row of blocks differs");
        checkParity(r, "mosaic scene matches on the GPU");
    }

    // --- the object window --------------------------------------------------
    std::printf("object window\n");
    {
        Scene s;
        disableAllObjects(s);
        poke16(s.pram, 2, kRed);
        poke16(s.pram, 4, kGreen);
        poke16(s.pram, kObjPalette + 2, 0x7FFF);
        fillTile4bpp(s, kTileBase + 0 * 32, 1);   // BG0 tile: red
        fillTile4bpp(s, kTileBase + 1 * 32, 2);   // BG1 tile: green
        for (uint32_t i = 0; i < 32 * 32; ++i) {
            poke16(s.vram, kBg0Map + i * 2, 0);
            poke16(s.vram, kBg1Map + i * 2, 1);
        }
        for (uint32_t t = 0; t < 4; ++t) fillTile4bpp(s, kObjTileBase + t * 32, 1);

        // Mode 0 with BG0, BG1, sprites and the object window enabled.
        poke16(s.io, 0x00, 0x0300u | 0x1040u | 0x8000u);
        poke16(s.io, 0x08, (28u << 8));
        poke16(s.io, 0x0A, (29u << 8) | 1u);   // BG1 lower priority
        poke16(s.io, 0x48, 0x0000);            // WININ unused
        poke16(s.io, 0x4A, 0x0002u | (0x0001u << 8));  // outside: BG1; obj window: BG0

        // A 16x16 sprite in object-window mode draws nothing; it only marks.
        poke16(s.oam, 0, 60u | (2u << 10));    // Y = 60, OBJ mode 2 = window
        poke16(s.oam, 2, 60u | (1u << 14));    // X = 60, 16x16
        poke16(s.oam, 4, 0);

        Result r = runScene(ctx, s);
        check(pixelAt(r.cpuFb, 68, 68) == kRed, "inside the object window BG0 shows");
        check(pixelAt(r.cpuFb, 10, 10) == kGreen, "outside it BG1 shows");
        check(pixelAt(r.cpuFb, 59, 68) == kGreen, "the window's left edge is exact");
        check(pixelAt(r.cpuFb, 60, 68) == kRed, "the window includes its first column");
        checkParity(r, "object window scene matches on the GPU");
    }

    // --- semi-transparent sprites -------------------------------------------
    std::printf("semi-transparent sprites\n");
    {
        Scene s;
        disableAllObjects(s);
        poke16(s.pram, 2, kGreen);                 // BG0
        poke16(s.pram, kObjPalette + 2, kRed);     // the sprite
        fillTile4bpp(s, kTileBase, 1);
        for (uint32_t i = 0; i < 32 * 32; ++i) poke16(s.vram, kBg0Map + i * 2, 0);
        for (uint32_t t = 0; t < 4; ++t) fillTile4bpp(s, kObjTileBase + t * 32, 1);

        poke16(s.io, 0x00, 0x0100u | 0x1040u);
        poke16(s.io, 0x08, (28u << 8) | 1u);       // BG0 below the sprite
        // BLDCNT names BG0 as the second target. The effect field stays at
        // "none": a semi-transparent sprite forces alpha regardless.
        poke16(s.io, 0x50, 0x0100u);
        poke16(s.io, 0x52, 8u | (8u << 8));        // EVA = EVB = 8/16

        poke16(s.oam, 0, 60u | (1u << 10));        // OBJ mode 1 = semi-transparent
        poke16(s.oam, 2, 60u | (1u << 14));
        poke16(s.oam, 4, 0);

        Result r = runScene(ctx, s);
        // Worked out here from the hardware formula, not from the renderer.
        auto blend = [](uint32_t top, uint32_t bot, uint32_t eva, uint32_t evb) {
            auto ch = [&](int sh) {
                return std::min(31u, (((top >> sh) & 31) * eva + ((bot >> sh) & 31) * evb) >> 4);
            };
            return ch(0) | (ch(5) << 5) | (ch(10) << 10);
        };
        const uint32_t want = blend(kRed, kGreen, 8, 8);
        const uint32_t got = pixelAt(r.cpuFb, 68, 68);
        if (got != want) std::printf("    got %04X want %04X\n", got, want);
        check(got == want, "a semi-transparent sprite blends with the layer below");
        check(pixelAt(r.cpuFb, 10, 10) == kGreen, "elsewhere the background is untouched");
        checkParity(r, "semi-transparent scene matches on the GPU");
    }

    ctx.destroy();
    if (failures) {
        std::printf("\nPPU COVERAGE FAILED: %d checks failed\n", failures);
        return 1;
    }
    std::printf("\nPPU COVERAGE PASS\n");
    return 0;
}
