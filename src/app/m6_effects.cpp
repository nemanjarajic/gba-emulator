// M6 gate for the parts of the PPU no available test ROM exercises: layer
// priority, windows, the colour effects, and affine sprites.
//
// Each scene is built directly in video memory, rendered for one frame on both
// the CPU reference and the GPU, and checked against a colour computed
// independently here. Where a check needs a blended value it is worked out from
// the hardware formula in this file rather than by calling the renderer, so the
// test cannot agree with the implementation by construction.

#include "core/core.inc"
#include "gpu/instance_pool.h"
#include "gpu/vk_context.h"
#include "host/memory.h"
#include "host/png.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

using namespace gba;

namespace {

constexpr uint32_t kLocalSize = 64;
constexpr uint32_t kInstances = 8;
int failures = 0;

constexpr uint32_t kRed = 0x001F;    // palette index 1, BG0
constexpr uint32_t kGreen = 0x03E0;  // palette index 2, BG1

// VRAM layout shared by every scene.
constexpr uint32_t kTileBase = 0x0000;    // character base block 0
constexpr uint32_t kBg0Map = 28u * 0x800; // screen base block 28
constexpr uint32_t kBg1Map = 29u * 0x800; // screen base block 29

void check(bool ok, const char* what) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

void poke16(std::vector<uint32_t>& region, uint32_t off, uint32_t value) {
    uint32_t& w = region[off >> 2];
    const uint32_t sh = (off & 2) * 8;
    w = (w & ~(0xFFFFu << sh)) | ((value & 0xFFFF) << sh);
}

uint32_t pixelAt(const std::vector<uint32_t>& fb, uint32_t x, uint32_t y) {
    const uint32_t i = y * SCREEN_W + x;
    return (fb[i >> 1] >> ((i & 1) * 16)) & 0xFFFF;
}

struct Scene {
    std::vector<uint32_t> vram = std::vector<uint32_t>(VRAM_WORDS, 0);
    std::vector<uint32_t> pram = std::vector<uint32_t>(PRAM_WORDS, 0);
    std::vector<uint32_t> oam = std::vector<uint32_t>(OAM_WORDS, 0);
    std::vector<uint32_t> io = std::vector<uint32_t>(IO_WORDS, 0);
};

// Two full-screen background layers: BG0 solid red, BG1 solid green.
Scene twoBackgroundScene(uint32_t bg0Priority, uint32_t bg1Priority) {
    Scene s;
    poke16(s.pram, 2, kRed);    // palette index 1
    poke16(s.pram, 4, kGreen);  // palette index 2

    // Tile 0 is solid index 1, tile 1 solid index 2. 4bpp, so one nibble each.
    for (uint32_t w = 0; w < 8; ++w) {
        s.vram[(kTileBase + 0 * 32) / 4 + w] = 0x11111111u;
        s.vram[(kTileBase + 1 * 32) / 4 + w] = 0x22222222u;
    }
    // Both maps are 32x32 entries of a single tile.
    for (uint32_t i = 0; i < 32 * 32; ++i) {
        poke16(s.vram, kBg0Map + i * 2, 0);
        poke16(s.vram, kBg1Map + i * 2, 1);
    }

    poke16(s.io, 0x00, 0x0300);  // mode 0, BG0 and BG1 enabled
    poke16(s.io, 0x08, bg0Priority | (28u << 8));
    poke16(s.io, 0x0A, bg1Priority | (29u << 8));
    return s;
}

// Disables every object, so a zeroed OAM does not render 128 stray 8x8 sprites.
void disableAllObjects(Scene& s) {
    for (uint32_t i = 0; i < 128; ++i) poke16(s.oam, i * 8 + 0, 1u << 9);
}

struct Result {
    std::vector<uint32_t> cpuFb, gpuFb;
};

Result runScene(VkContext& ctx, const Scene& scene) {
    const std::vector<uint32_t> rom{0xEAFFFFFEu};  // B . -- the CPU does nothing

    host::MemoryPool hostPool;
    hostPool.allocate(1, uint32_t(rom.size()), /*withFramebuffers=*/true);
    hostPool.bind();
    host::installBios(hostPool.bios);
    hostPool.io[REG_KEYINPUT >> 2] = 0x03FF;  // KEYINPUT is active low: no keys held
    hostPool.rom[0] = rom[0];
    std::copy(scene.vram.begin(), scene.vram.end(), hostPool.vram.begin());
    std::copy(scene.pram.begin(), scene.pram.end(), hostPool.pram.begin());
    std::copy(scene.oam.begin(), scene.oam.end(), hostPool.oam.begin());
    std::copy(scene.io.begin(), scene.io.end(), hostPool.io.begin());
    g_flags = FLAG_RENDER;

    GbaState cpuState{};
    host::hleBoot(cpuState);
    step_cycles(cpuState, CYCLES_PER_FRAME);

    Result r;
    r.cpuFb.assign(hostPool.fb.begin(), hostPool.fb.begin() + FB_WORDS);

    InstancePool pool;
    pool.create(ctx, kInstances, uint32_t(rom.size()), /*withFramebuffers=*/true);
    uploadBuffer(ctx, pool.rom, rom.data(), rom.size() * 4);
    for (uint32_t i = 0; i < kInstances; ++i) {
        uploadBuffer(ctx, pool.vram, scene.vram.data(), scene.vram.size() * 4,
                     VkDeviceSize(i) * VRAM_WORDS * 4);
        uploadBuffer(ctx, pool.pram, scene.pram.data(), scene.pram.size() * 4,
                     VkDeviceSize(i) * PRAM_WORDS * 4);
        uploadBuffer(ctx, pool.oam, scene.oam.data(), scene.oam.size() * 4,
                     VkDeviceSize(i) * OAM_WORDS * 4);
        uploadBuffer(ctx, pool.io, scene.io.data(), scene.io.size() * 4,
                     VkDeviceSize(i) * IO_WORDS * 4);
    }

    std::vector<GbaState> states(kInstances);
    for (uint32_t i = 0; i < kInstances; ++i) {
        states[i] = GbaState{};
        host::hleBoot(states[i]);
        states[i].inst = i;
    }
    pool.uploadStates(ctx, states);

    ComputePipeline pipe;
    pipe.create(ctx, std::string(SHADER_DIR) + "/gba.spv", uint32_t(pool.bindings().size()),
                sizeof(CorePush));
    pipe.bindBuffers(ctx, pool.bindings());
    for (uint32_t done = 0; done < CYCLES_PER_FRAME;) {
        const uint32_t chunk = std::min(16384u, CYCLES_PER_FRAME - done);
        CorePush push{kInstances, uint32_t(rom.size()), chunk, FLAG_RENDER};
        dispatchBlocking(ctx, pipe, (kInstances + kLocalSize - 1) / kLocalSize, &push,
                         sizeof(push));
        done += chunk;
    }
    r.gpuFb.resize(FB_WORDS);
    downloadBuffer(ctx, pool.fb, r.gpuFb.data(), FB_WORDS * 4);

    pipe.destroy(ctx);
    pool.destroy(ctx);
    // The context is owned by main and shared across scenes; destroying it here
    // would invalidate the device for every later scene.
    return r;
}

void checkParity(const Result& r, const char* label) {
    size_t diff = 0;
    for (uint32_t i = 0; i < FB_WORDS; ++i)
        if (r.cpuFb[i] != r.gpuFb[i]) ++diff;
    if (diff) std::printf("    %zu/%u words differ\n", diff, FB_WORDS);
    check(diff == 0, label);
}

// The hardware blend formulas, written out here independently of the renderer.
uint32_t expectAlpha(uint32_t top, uint32_t bottom, uint32_t eva, uint32_t evb) {
    auto ch = [&](int shift) {
        const uint32_t a = (top >> shift) & 31, b = (bottom >> shift) & 31;
        return std::min(31u, (a * eva + b * evb) >> 4);
    };
    return ch(0) | (ch(5) << 5) | (ch(10) << 10);
}

uint32_t expectBrightness(uint32_t c, uint32_t evy, bool up) {
    auto ch = [&](int shift) {
        const uint32_t v = (c >> shift) & 31;
        return up ? v + (((31 - v) * evy) >> 4) : v - ((v * evy) >> 4);
    };
    return ch(0) | (ch(5) << 5) | (ch(10) << 10);
}

}  // namespace

int main() {
    VkContext ctx;
    ctx.init(/*validation=*/true, /*debugPrintf=*/false);

    // --- layer priority -----------------------------------------------------
    {
        Scene s = twoBackgroundScene(/*bg0=*/0, /*bg1=*/1);
        disableAllObjects(s);
        Result r = runScene(ctx, s);
        check(pixelAt(r.cpuFb, 120, 80) == kRed, "BG0 at priority 0 covers BG1 at priority 1");
        checkParity(r, "priority scene matches on the GPU");
    }
    {
        Scene s = twoBackgroundScene(/*bg0=*/1, /*bg1=*/0);
        disableAllObjects(s);
        Result r = runScene(ctx, s);
        check(pixelAt(r.cpuFb, 120, 80) == kGreen, "swapping priorities puts BG1 on top");
        checkParity(r, "swapped priority scene matches on the GPU");
    }

    // --- windows ------------------------------------------------------------
    {
        Scene s = twoBackgroundScene(/*bg0=*/0, /*bg1=*/1);
        disableAllObjects(s);
        poke16(s.io, 0x00, 0x2300);        // ...plus WIN0 enabled
        poke16(s.io, 0x40, (40u << 8) | 120u);  // WIN0H: x from 40 to 120
        poke16(s.io, 0x44, (40u << 8) | 120u);  // WIN0V: y from 40 to 120
        poke16(s.io, 0x48, 0x0001);        // inside WIN0: BG0 only
        poke16(s.io, 0x4A, 0x0002);        // outside: BG1 only
        Result r = runScene(ctx, s);
        check(pixelAt(r.cpuFb, 80, 80) == kRed, "inside WIN0 only BG0 is visible");
        check(pixelAt(r.cpuFb, 10, 10) == kGreen, "outside WIN0 only BG1 is visible");
        check(pixelAt(r.cpuFb, 39, 80) == kGreen, "WIN0 left edge is exclusive of x1-1");
        check(pixelAt(r.cpuFb, 40, 80) == kRed, "WIN0 left edge includes x1");
        check(pixelAt(r.cpuFb, 119, 80) == kRed, "WIN0 right edge includes x2-1");
        check(pixelAt(r.cpuFb, 120, 80) == kGreen, "WIN0 right edge excludes x2");
        checkParity(r, "window scene matches on the GPU");
        host::writePng("fx_window.png", host::bgr555ToRgb(r.gpuFb, SCREEN_W, SCREEN_H), SCREEN_W,
                       SCREEN_H);
    }

    // --- alpha blending -----------------------------------------------------
    {
        Scene s = twoBackgroundScene(/*bg0=*/0, /*bg1=*/1);
        disableAllObjects(s);
        poke16(s.io, 0x50, 0x0201 | (1u << 6));  // target1 BG0, effect alpha, target2 BG1
        poke16(s.io, 0x52, 8u | (8u << 8));      // EVA = EVB = 8/16
        Result r = runScene(ctx, s);
        const uint32_t want = expectAlpha(kRed, kGreen, 8, 8);
        const uint32_t got = pixelAt(r.cpuFb, 120, 80);
        if (got != want) std::printf("    got %04X want %04X\n", got, want);
        check(got == want, "alpha blends BG0 over BG1 at EVA=EVB=8/16");
        checkParity(r, "alpha scene matches on the GPU");
    }

    // --- brightness ---------------------------------------------------------
    for (int up = 1; up >= 0; --up) {
        Scene s = twoBackgroundScene(/*bg0=*/0, /*bg1=*/1);
        disableAllObjects(s);
        poke16(s.io, 0x50, 0x0001 | ((up ? 2u : 3u) << 6));  // target1 BG0
        poke16(s.io, 0x54, 8u);                              // EVY = 8/16
        Result r = runScene(ctx, s);
        const uint32_t want = expectBrightness(kRed, 8, up != 0);
        const uint32_t got = pixelAt(r.cpuFb, 120, 80);
        if (got != want) std::printf("    got %04X want %04X\n", got, want);
        check(got == want, up ? "brightness increase on BG0" : "brightness decrease on BG0");
        checkParity(r, up ? "brighten scene matches on the GPU"
                          : "darken scene matches on the GPU");
    }

    // --- affine sprites -----------------------------------------------------
    //
    // An identity matrix must reproduce the non-affine sprite exactly. That is
    // the strongest available check on the rotation path, because any error in
    // the centre offsets or the fixed-point shift shows up as a displacement.
    {
        Scene base;
        disableAllObjects(base);
        poke16(base.pram, 0x200 + 2, kRed);
        for (uint32_t w = 0; w < 8; ++w) base.vram[(0x10000) / 4 + w] = 0x11111111u;
        poke16(base.io, 0x00, 0x1040);  // mode 0, OBJ on, 1D mapping

        Scene plain = base;
        poke16(plain.oam, 0, 40u);                      // Y = 40, square, 4bpp
        poke16(plain.oam, 2, 60u | (1u << 14));         // X = 60, size 1 => 16x16
        poke16(plain.oam, 4, 0);
        Result rp = runScene(ctx, plain);

        Scene affine = base;
        poke16(affine.oam, 0, 40u | (1u << 8));         // Y = 40, rotation/scaling on
        poke16(affine.oam, 2, 60u | (1u << 14));        // X = 60, matrix 0
        poke16(affine.oam, 4, 0);
        poke16(affine.oam, 0x06, 0x0100);               // PA = 1.0
        poke16(affine.oam, 0x0E, 0x0000);               // PB = 0
        poke16(affine.oam, 0x16, 0x0000);               // PC = 0
        poke16(affine.oam, 0x1E, 0x0100);               // PD = 1.0
        Result ra = runScene(ctx, affine);

        size_t diff = 0;
        for (uint32_t i = 0; i < FB_WORDS; ++i)
            if (rp.cpuFb[i] != ra.cpuFb[i]) ++diff;
        if (diff) std::printf("    %zu/%u words differ from the non-affine sprite\n", diff,
                              FB_WORDS);
        check(diff == 0, "affine sprite with an identity matrix matches a plain one");
        checkParity(ra, "affine sprite scene matches on the GPU");
        host::writePng("fx_affine.png", host::bgr555ToRgb(ra.gpuFb, SCREEN_W, SCREEN_H), SCREEN_W,
                       SCREEN_H);
    }

    // --- affine backgrounds -------------------------------------------------
    //
    // Mode 2 with an identity matrix should paint the map's top-left 128x128
    // corner one-to-one onto the screen. The display-area-overflow bit then
    // decides what the rest of the screen shows, which is the cheapest way to
    // confirm the coordinate wrap is actually wired up.
    for (int wrap = 0; wrap <= 1; ++wrap) {
        Scene s;
        disableAllObjects(s);
        poke16(s.pram, 2, kRed);  // palette index 1

        // Affine tiles are always 256 colours: one byte per pixel, 64 per tile.
        for (uint32_t w = 0; w < 16; ++w) s.vram[(kTileBase + 0 * 64) / 4 + w] = 0x01010101u;
        // The affine map is one byte per tile; a zeroed map already selects
        // tile 0 everywhere, which is what we want.

        poke16(s.io, 0x00, 0x0402);  // mode 2, BG2 enabled
        poke16(s.io, 0x0C, (28u << 8) | (wrap ? 0x2000u : 0u));  // BG2CNT, size 0 => 128x128
        poke16(s.io, 0x20, 0x0100);  // PA = 1.0
        poke16(s.io, 0x22, 0x0000);  // PB
        poke16(s.io, 0x24, 0x0000);  // PC
        poke16(s.io, 0x26, 0x0100);  // PD
        s.io[0x28 >> 2] = 0;         // reference X
        s.io[0x2C >> 2] = 0;         // reference Y

        Result r = runScene(ctx, s);
        check(pixelAt(r.cpuFb, 60, 60) == kRed, wrap ? "affine BG inside the map (wrap on)"
                                                     : "affine BG inside the map (wrap off)");
        const uint32_t outside = pixelAt(r.cpuFb, 200, 140);
        check(outside == (wrap ? kRed : 0u),
              wrap ? "affine BG wraps past the map edge"
                   : "affine BG shows backdrop past the map edge");
        checkParity(r, wrap ? "affine BG wrap scene matches on the GPU"
                            : "affine BG no-wrap scene matches on the GPU");
        if (!wrap)
            host::writePng("fx_affine_bg.png", host::bgr555ToRgb(r.gpuFb, SCREEN_W, SCREEN_H),
                           SCREEN_W, SCREEN_H);
    }

    ctx.destroy();
    if (failures) {
        std::printf("M6 EFFECTS FAILED: %d checks failed\n", failures);
        return 1;
    }
    std::printf("M6 EFFECTS PASS\n");
    return 0;
}
