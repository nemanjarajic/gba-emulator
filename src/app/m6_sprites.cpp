// M6 sprite gate.
//
// jsmolka's suite has no OBJ test, so this builds sprite scenes directly:
// DISPCNT, the object palette, OBJ tiles and OAM are written from the host and
// one frame is rendered.
//
// Driving this from a hand-assembled ROM (as M5 does for the bitmap modes)
// would test the CPU's writes to OAM and VRAM, which M1 and M2 already cover
// thoroughly, while making it far harder to sweep sprite configurations. What
// is new and untested here is the sprite *renderer*, so the setup is poked
// directly and many configurations are checked instead of one.
//
// Each sprite is a 16x16 block of four uniform quadrants using palette indices
// 1, 2, 3, 4 (top-left, top-right, bottom-left, bottom-right). That asymmetry
// is what makes a flip observable: a horizontal flip must swap 1 with 2 and 3
// with 4, and a vertical flip must swap 1 with 3 and 2 with 4. A symmetric
// test pattern would pass whether or not flipping worked.

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
constexpr uint32_t kInstances = 16;
int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

// Quadrant colours, chosen to be unmistakable from each other and the backdrop.
const uint32_t kQuadColour[5] = {0x0000, 0x001F, 0x03E0, 0x7C00, 0x7FFF};

// Little helpers for poking a region held as a vector of words.
void poke16(std::vector<uint32_t>& region, uint32_t byteOffset, uint32_t value) {
    uint32_t& w = region[byteOffset >> 2];
    const uint32_t sh = (byteOffset & 2) * 8;
    w = (w & ~(0xFFFFu << sh)) | ((value & 0xFFFF) << sh);
}

struct Sprite {
    const char* name;
    uint32_t x, y;
    bool hflip, vflip;
};

// The four quadrant indices a sprite shows, after its flips.
void expectedQuadrants(const Sprite& s, uint32_t out[4]) {
    uint32_t q[4] = {1, 2, 3, 4};  // TL, TR, BL, BR
    if (s.hflip) { std::swap(q[0], q[1]); std::swap(q[2], q[3]); }
    if (s.vflip) { std::swap(q[0], q[2]); std::swap(q[1], q[3]); }
    for (int i = 0; i < 4; ++i) out[i] = q[i];
}

uint32_t pixelAt(const std::vector<uint32_t>& fb, uint32_t x, uint32_t y) {
    const uint32_t i = y * SCREEN_W + x;
    return (fb[i >> 1] >> ((i & 1) * 16)) & 0xFFFF;
}

}  // namespace

int main() {
    const Sprite sprites[4] = {
        {"no flip",   20, 40, false, false},
        {"hflip",     60, 40, true,  false},
        {"vflip",    100, 40, false, true },
        {"both flips",140, 40, true,  true },
    };

    // ---- build the scene ---------------------------------------------------
    std::vector<uint32_t> vram(VRAM_WORDS, 0), pram(PRAM_WORDS, 0), oam(OAM_WORDS, 0),
        io(IO_WORDS, 0);

    // Mode 0, no backgrounds, sprites on, one-dimensional tile mapping.
    poke16(io, 0x00, 0x1040);

    // Object palette occupies the second half of palette RAM.
    for (uint32_t i = 1; i <= 4; ++i) poke16(pram, 0x200 + i * 2, kQuadColour[i]);

    // Four 4bpp tiles, each a solid palette index, arranged 2x2 by the
    // one-dimensional mapping into a 16x16 sprite.
    for (uint32_t tile = 0; tile < 4; ++tile) {
        const uint32_t idx = tile + 1;
        const uint32_t packed = idx * 0x11111111u;  // eight pixels per word
        for (uint32_t w = 0; w < 8; ++w) vram[(0x10000 + tile * 32) / 4 + w] = packed;
    }

    for (uint32_t i = 0; i < 4; ++i) {
        const Sprite& s = sprites[i];
        poke16(oam, i * 8 + 0, s.y & 0xFF);                       // square, 4bpp
        poke16(oam, i * 8 + 2, (s.x & 0x1FF) | (1u << 14) |       // size 1 => 16x16
                                   (s.hflip ? (1u << 12) : 0) | (s.vflip ? (1u << 13) : 0));
        poke16(oam, i * 8 + 4, 0);                                // tile 0, priority 0
    }
    // Every other object must be switched off: a zeroed OAM entry is a valid
    // enabled 8x8 sprite at (0,0), not an absent one.
    for (uint32_t i = 4; i < 128; ++i) poke16(oam, i * 8 + 0, 1u << 9);

    const std::vector<uint32_t> rom{0xEAFFFFFEu};  // B . -- the CPU does nothing

    // ---- CPU reference -----------------------------------------------------
    host::MemoryPool hostPool;
    hostPool.allocate(1, uint32_t(rom.size()), /*withFramebuffers=*/true);
    hostPool.bind();
    host::installBios(hostPool.bios);
    hostPool.io[REG_KEYINPUT >> 2] = 0x03FF;  // KEYINPUT is active low: no keys held
    hostPool.rom[0] = rom[0];
    std::copy(vram.begin(), vram.end(), hostPool.vram.begin());
    std::copy(pram.begin(), pram.end(), hostPool.pram.begin());
    std::copy(oam.begin(), oam.end(), hostPool.oam.begin());
    std::copy(io.begin(), io.end(), hostPool.io.begin());
    g_flags = FLAG_RENDER;

    GbaState cpuState{};
    host::hleBoot(cpuState);
    step_cycles(cpuState, CYCLES_PER_FRAME);

    const std::vector<uint32_t> cpuFb(hostPool.fb.begin(), hostPool.fb.begin() + FB_WORDS);

    // ---- checks ------------------------------------------------------------
    for (const Sprite& s : sprites) {
        uint32_t q[4];
        expectedQuadrants(s, q);
        // Sample the middle of each quadrant.
        const uint32_t sx[4] = {s.x + 4, s.x + 12, s.x + 4, s.x + 12};
        const uint32_t sy[4] = {s.y + 4, s.y + 4, s.y + 12, s.y + 12};
        bool ok = true;
        for (int k = 0; k < 4; ++k) {
            const uint32_t got = pixelAt(cpuFb, sx[k], sy[k]);
            if (got != kQuadColour[q[k]]) {
                std::printf("    %s quadrant %d at (%u,%u): got %04X want %04X\n", s.name, k,
                            sx[k], sy[k], got, kQuadColour[q[k]]);
                ok = false;
            }
        }
        check(ok, (std::string("sprite \"") + s.name + "\" draws the right quadrants").c_str());
    }

    // Just outside each sprite must still be backdrop, which catches an
    // off-by-one in the bounding box.
    bool edges = true;
    for (const Sprite& s : sprites) {
        if (pixelAt(cpuFb, s.x - 1, s.y + 8) != 0) edges = false;
        if (pixelAt(cpuFb, s.x + 16, s.y + 8) != 0) edges = false;
        if (pixelAt(cpuFb, s.x + 8, s.y - 1) != 0) edges = false;
        if (pixelAt(cpuFb, s.x + 8, s.y + 16) != 0) edges = false;
    }
    check(edges, "sprites do not bleed outside their 16x16 box");

    // The disabled objects must not have drawn anything at the origin.
    check(pixelAt(cpuFb, 2, 2) == 0, "disabled objects draw nothing");

    // ---- GPU ---------------------------------------------------------------
    VkContext ctx;
    ctx.init(/*validation=*/true, /*debugPrintf=*/false);
    InstancePool pool;
    pool.create(ctx, kInstances, uint32_t(rom.size()), /*withFramebuffers=*/true);
    uploadBuffer(ctx, pool.rom, rom.data(), rom.size() * 4);
    for (uint32_t i = 0; i < kInstances; ++i) {
        uploadBuffer(ctx, pool.vram, vram.data(), vram.size() * 4, VkDeviceSize(i) * VRAM_WORDS * 4);
        uploadBuffer(ctx, pool.pram, pram.data(), pram.size() * 4, VkDeviceSize(i) * PRAM_WORDS * 4);
        uploadBuffer(ctx, pool.oam, oam.data(), oam.size() * 4, VkDeviceSize(i) * OAM_WORDS * 4);
        uploadBuffer(ctx, pool.io, io.data(), io.size() * 4, VkDeviceSize(i) * IO_WORDS * 4);
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

    std::vector<uint32_t> gpuFb(FB_WORDS);
    downloadBuffer(ctx, pool.fb, gpuFb.data(), FB_WORDS * 4);

    size_t diff = 0;
    for (uint32_t i = 0; i < FB_WORDS; ++i)
        if (cpuFb[i] != gpuFb[i]) ++diff;
    if (diff) std::printf("    %zu/%u framebuffer words differ\n", diff, FB_WORDS);
    check(diff == 0, "GPU framebuffer matches the CPU reference");

    check(host::writePng("sprites.png", host::bgr555ToRgb(gpuFb, SCREEN_W, SCREEN_H), SCREEN_W,
                         SCREEN_H),
          "wrote sprites.png");

    pipe.destroy(ctx);
    pool.destroy(ctx);
    ctx.destroy();

    if (failures) {
        std::printf("M6 SPRITES FAILED: %d checks failed\n", failures);
        return 1;
    }
    std::printf("M6 SPRITES PASS\n");
    return 0;
}
