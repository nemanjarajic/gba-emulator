// Renders a ROM on both the CPU reference and the GPU, compares the two
// framebuffers, and writes a PNG.
//
// This is the M6 gate and a general-purpose tool: point it at any ROM to see
// what the emulator makes of it.
//
//   render_rom <rom> [frames] [out.png]

#include "core/core.inc"
#include "gpu/instance_pool.h"
#include "gpu/vk_context.h"
#include "host/memory.h"
#include "host/png.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace gba;

namespace {

constexpr uint32_t kLocalSize = 64;
constexpr uint32_t kInstances = 16;

size_t countDifferences(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    size_t n = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (a[i] != b[i]) ++n;
    return n;
}

uint32_t distinctColours(const std::vector<uint32_t>& fb) {
    std::vector<uint8_t> seen(65536, 0);
    uint32_t n = 0;
    for (uint32_t i = 0; i < SCREEN_W * SCREEN_H; ++i) {
        const uint32_t c = (fb[i >> 1] >> ((i & 1) * 16)) & 0xFFFF;
        if (!seen[c]) { seen[c] = 1; ++n; }
    }
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: render_rom <rom> [frames] [out.png]\n");
        return 2;
    }
    const std::string romPath = argv[1];
    const uint32_t frames = argc > 2 ? uint32_t(std::atoi(argv[2])) : 4;
    const std::string outPath = argc > 3 ? argv[3] : "frame.png";

    std::vector<uint32_t> rom;
    if (!host::loadBinary(romPath, rom)) {
        std::fprintf(stderr, "cannot read %s\n", romPath.c_str());
        return 2;
    }
    const uint32_t totalCycles = frames * CYCLES_PER_FRAME;

    // ---- CPU reference -----------------------------------------------------
    host::MemoryPool hostPool;
    hostPool.allocate(1, uint32_t(rom.size()), /*withFramebuffers=*/true);
    hostPool.bind();
    std::copy(rom.begin(), rom.end(), hostPool.rom.begin());
    g_flags = FLAG_RENDER;

    GbaState cpuState{};
    host::hleBoot(cpuState);
    for (uint32_t i = 0; i < totalCycles; ++i) cpu_step(cpuState);

    const std::vector<uint32_t> cpuFb(hostPool.fb.begin(), hostPool.fb.begin() + FB_WORDS);
    std::printf("%s: %u frames (%u cycles)\n", romPath.c_str(), frames, totalCycles);
    std::printf("  DISPCNT = 0x%04X (mode %u), %u distinct colours\n",
                hostPool.io[0] & 0xFFFF, hostPool.io[0] & 7, distinctColours(cpuFb));

    // ---- GPU ---------------------------------------------------------------
    VkContext ctx;
    ctx.init(/*validation=*/true, /*debugPrintf=*/false);
    InstancePool pool;
    pool.create(ctx, kInstances, uint32_t(rom.size()), /*withFramebuffers=*/true);
    uploadBuffer(ctx, pool.rom, rom.data(), rom.size() * 4);

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

    for (uint32_t done = 0; done < totalCycles;) {
        const uint32_t chunk = std::min(16384u, totalCycles - done);
        CorePush push{kInstances, uint32_t(rom.size()), chunk, FLAG_RENDER};
        dispatchBlocking(ctx, pipe, (kInstances + kLocalSize - 1) / kLocalSize, &push,
                         sizeof(push));
        done += chunk;
    }

    std::vector<uint32_t> gpuFb(FB_WORDS);
    downloadBuffer(ctx, pool.fb, gpuFb.data(), FB_WORDS * 4);

    const size_t diff = countDifferences(cpuFb, gpuFb);
    std::printf("  CPU/GPU framebuffer difference: %zu/%u words\n", diff, FB_WORDS);

    const bool wrote =
        host::writePng(outPath, host::bgr555ToRgb(gpuFb, SCREEN_W, SCREEN_H), SCREEN_W, SCREEN_H);
    std::printf("  wrote %s\n", outPath.c_str());

    pipe.destroy(ctx);
    pool.destroy(ctx);
    ctx.destroy();

    if (diff != 0) { std::printf("  FAIL: GPU disagrees with the CPU reference\n"); return 1; }
    if (!wrote) { std::printf("  FAIL: could not write %s\n", outPath.c_str()); return 1; }
    return 0;
}
