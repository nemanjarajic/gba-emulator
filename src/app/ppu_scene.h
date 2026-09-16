#pragma once

// Shared harness for the PPU scene tests.
//
// A "scene" is video memory built directly by the host rather than by a ROM:
// the PPU features these exercise -- windows, blending, mosaic, sprite
// geometry -- are renderer behaviour, and the CPU's writes to OAM and VRAM are
// already covered thoroughly by M1 and M2. Poking the setup directly makes it
// practical to sweep many configurations instead of hand-assembling a ROM for
// each one.
//
// Every scene is rendered for one frame on both the CPU reference and the GPU
// and the two framebuffers compared, so each test is also a parity test.

#include "core/core.inc"
#include "gpu/instance_pool.h"
#include "gpu/vk_context.h"
#include "host/memory.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace gba::scene {

constexpr uint32_t kLocalSize = 64;
constexpr uint32_t kInstances = 8;
inline int failures = 0;

constexpr uint32_t kRed = 0x001F;    // palette index 1, BG0
constexpr uint32_t kGreen = 0x03E0;  // palette index 2, BG1

// VRAM layout shared by every scene.
constexpr uint32_t kTileBase = 0x0000;    // character base block 0
constexpr uint32_t kBg0Map = 28u * 0x800; // screen base block 28
constexpr uint32_t kBg1Map = 29u * 0x800; // screen base block 29

inline void check(bool ok, const char* what) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

inline void poke16(std::vector<uint32_t>& region, uint32_t off, uint32_t value) {
    uint32_t& w = region[off >> 2];
    const uint32_t sh = (off & 2) * 8;
    w = (w & ~(0xFFFFu << sh)) | ((value & 0xFFFF) << sh);
}

inline uint32_t pixelAt(const std::vector<uint32_t>& fb, uint32_t x, uint32_t y) {
    const uint32_t i = y * SCREEN_W + x;
    return (fb[i >> 1] >> ((i & 1) * 16)) & 0xFFFF;
}

struct Scene {
    std::vector<uint32_t> vram = std::vector<uint32_t>(VRAM_WORDS, 0);
    std::vector<uint32_t> pram = std::vector<uint32_t>(PRAM_WORDS, 0);
    std::vector<uint32_t> oam = std::vector<uint32_t>(OAM_WORDS, 0);
    std::vector<uint32_t> io = std::vector<uint32_t>(IO_WORDS, 0);
};


// Disables every object, so a zeroed OAM does not render 128 stray 8x8 sprites
// at the origin.
inline void disableAllObjects(Scene& s) {
    for (uint32_t i = 0; i < 128; ++i) poke16(s.oam, i * 8 + 0, 1u << 9);
}

struct Result {
    std::vector<uint32_t> cpuFb, gpuFb;
};

inline Result runScene(VkContext& ctx, const Scene& scene) {
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

inline void checkParity(const Result& r, const char* label) {
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

}  // namespace gba::scene
