// M4 gate: run a CPU test ROM inside the compute shader and verify it against
// the C++ reference, instance for instance and byte for byte.
//
// Three things are checked, in increasing strength:
//   1. The ROM's own verdict (r12 == 0) as computed ON THE GPU.
//   2. The GPU's final machine state matches the C++ reference exactly.
//   3. Every instance agrees with instance 0 -- the determinism property that
//      M9's throughput harness depends on, and the one that would expose a
//      MEM_IDX bug letting instances scribble on each other.

#include "core/core.inc"
#include "gpu/instance_pool.h"
#include "gpu/vk_context.h"
#include "host/memory.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace gba;

namespace {

constexpr uint32_t kLocalSize = 64;  // must match gba.comp
constexpr uint32_t kCyclesPerDispatch = 16384;
constexpr uint64_t kMaxInstructions = 200ull * 1000 * 1000;

int failures = 0;

void check(bool ok, const char* what) {
    if (ok) return;
    std::printf("  FAIL %s\n", what);
    ++failures;
}

// Compares one instance's slice of a region against the reference copy.
bool compareRegion(VkContext& ctx, Buffer& buf, const std::vector<uint32_t>& reference,
                   uint32_t wordsPerInstance, const char* name) {
    std::vector<uint32_t> got(wordsPerInstance);
    downloadBuffer(ctx, buf, got.data(), wordsPerInstance * 4,
                   /*srcOffset=*/0);  // instance 0 occupies the first slice
    for (uint32_t i = 0; i < wordsPerInstance; ++i) {
        if (got[i] == reference[i]) continue;
        std::printf("  FAIL %s differs at word 0x%X: GPU %08X, CPU %08X\n", name, i, got[i],
                    reference[i]);
        return false;
    }
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string romPath = argc > 1 ? argv[1] : "third_party/gba-tests/arm/arm.gba";
    const uint32_t instances = argc > 2 ? uint32_t(std::atoi(argv[2])) : 64;

    std::vector<uint32_t> rom;
    if (!host::loadBinary(romPath, rom)) {
        std::fprintf(stderr, "cannot read %s\n", romPath.c_str());
        return 2;
    }

    // ---- reference run on the CPU -----------------------------------------
    host::MemoryPool hostPool;
    hostPool.allocate(1, uint32_t(rom.size()), /*withFramebuffers=*/true);
    hostPool.bind();
    host::installBios(hostPool.bios);
    hostPool.io[REG_KEYINPUT >> 2] = 0x03FF;  // KEYINPUT is active low: no keys held
    std::copy(rom.begin(), rom.end(), hostPool.rom.begin());

    GbaState cpuState{};
    host::hleBoot(cpuState);

    const auto cpuT0 = std::chrono::steady_clock::now();
    uint64_t cycles = 0;
    bool finished = false;
    for (; cycles < kMaxInstructions; ++cycles) {
        const uint32_t pc = cpuState.r[15];
        cpu_step(cpuState);
        if (cpuState.r[15] == pc) { finished = true; break; }
    }
    if (!finished) {
        std::printf("reference run did not terminate\n");
        return 1;
    }
    // The self-branch is detected one step after it executes; the GPU must run
    // the same number of cycles to land in the same place.
    const double cpuSecs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - cpuT0).count();
    const uint32_t totalCycles = uint32_t(cycles) + 1;
    const double cpuRate = double(totalCycles) / cpuSecs / 1e6;
    std::printf("reference: %s finished in %u cycles, r12 = %u\n", romPath.c_str(), totalCycles,
                cpuState.r[12]);
    std::printf("CPU baseline: %.1f Mcycle/s on one core (%.0fx realtime)\n", cpuRate,
                cpuRate / 16.78);

    // ---- GPU run -----------------------------------------------------------
    VkContext ctx;
    ctx.init(/*validation=*/true, /*debugPrintf=*/false);

    InstancePool pool;
    pool.create(ctx, instances, uint32_t(rom.size()), /*withFramebuffers=*/true);
    uploadBuffer(ctx, pool.rom, rom.data(), rom.size() * 4);
    // The GPU needs the same BIOS: the interrupt path runs real ARM code from
    // the vector table, so a zeroed BIOS region would diverge from the CPU.
    uploadBuffer(ctx, pool.bios, hostPool.bios.data(), hostPool.bios.size() * 4);

    std::vector<GbaState> states(instances);
    for (uint32_t i = 0; i < instances; ++i) {
        states[i] = GbaState{};
        host::hleBoot(states[i]);
        states[i].inst = i;
    }
    pool.uploadStates(ctx, states);

    ComputePipeline pipe;
    pipe.create(ctx, std::string(SHADER_DIR) + "/gba.spv",
                uint32_t(pool.bindings().size()), sizeof(CorePush));
    pipe.bindBuffers(ctx, pool.bindings());

    std::printf("GPU: %u instances, %.1f MiB, %s memory\n", instances,
                double(pool.totalBytes()) / (1024.0 * 1024.0),
                pool.ewram.mapped ? "host-mapped device-local" : "device-local + staging");

    const auto t0 = std::chrono::steady_clock::now();
    uint32_t done = 0;
    uint32_t dispatches = 0;
    while (done < totalCycles) {
        const uint32_t chunk = std::min(kCyclesPerDispatch, totalCycles - done);
        CorePush push{instances, uint32_t(rom.size()), chunk, 0};
        dispatchBlocking(ctx, pipe, (instances + kLocalSize - 1) / kLocalSize, &push, sizeof(push));
        done += chunk;
        ++dispatches;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    pool.downloadStates(ctx, states);

    // ---- checks ------------------------------------------------------------
    std::printf("ran %u cycles in %u dispatches, %.2f s\n", totalCycles, dispatches, secs);
    const double gpuRate = double(totalCycles) * instances / secs / 1e6;
    std::printf("GPU: %.1f Mcycle/s aggregate, %.2f MHz per instance (hardware is 16.78)\n",
                gpuRate, gpuRate / instances);
    std::printf("  %.0fx realtime in aggregate, %.1fx one CPU core, %.3fx per instance\n",
                gpuRate / 16.78, gpuRate / cpuRate, (gpuRate / instances) / cpuRate);

    check(states[0].r[12] == 0, "ROM verdict on GPU (r12 != 0 means a test failed)");

    // Full register-file comparison against the reference.
    bool regsMatch = true;
    for (uint32_t i = 0; i < 16; ++i) {
        if (states[0].r[i] == cpuState.r[i]) continue;
        std::printf("  FAIL r%u: GPU %08X, CPU %08X\n", i, states[0].r[i], cpuState.r[i]);
        regsMatch = false;
    }
    check(regsMatch, "register file matches reference");
    check(states[0].cpsr == cpuState.cpsr, "cpsr matches reference");
    check(states[0].cycles == cpuState.cycles, "cycle count matches reference");
    check(states[0].scanline == cpuState.scanline, "scanline matches reference");

    // Byte-for-byte memory comparison. A CPU bug that only shows up through
    // memory -- a wrong store width, a wrong address -- would pass a register
    // check and fail here.
    check(compareRegion(ctx, pool.ewram, hostPool.ewram, EWRAM_WORDS, "EWRAM"), "EWRAM matches");
    check(compareRegion(ctx, pool.iwram, hostPool.iwram, IWRAM_WORDS, "IWRAM"), "IWRAM matches");
    check(compareRegion(ctx, pool.vram, hostPool.vram, VRAM_WORDS, "VRAM"), "VRAM matches");
    check(compareRegion(ctx, pool.pram, hostPool.pram, PRAM_WORDS, "PRAM"), "PRAM matches");
    check(compareRegion(ctx, pool.oam, hostPool.oam, OAM_WORDS, "OAM"), "OAM matches");
    check(compareRegion(ctx, pool.io, hostPool.io, IO_WORDS, "IO"), "IO matches");

    // Determinism across instances.
    uint32_t divergent = 0;
    for (uint32_t i = 1; i < instances; ++i) {
        GbaState a = states[0], b = states[i];
        a.inst = b.inst = 0;  // the one field that is legitimately different
        if (std::memcmp(&a, &b, sizeof(GbaState)) != 0) ++divergent;
    }
    if (divergent) std::printf("  FAIL %u/%u instances diverged from instance 0\n", divergent,
                               instances - 1);
    check(divergent == 0, "all instances produced identical state");

    pipe.destroy(ctx);
    pool.destroy(ctx);
    ctx.destroy();

    if (failures) {
        std::printf("M4 FAILED: %d checks failed\n", failures);
        return 1;
    }
    std::printf("M4 PASS: %s runs on the GPU with zero divergence from the CPU reference\n",
                romPath.c_str());
    return 0;
}
