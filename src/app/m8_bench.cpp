// M8 benchmark: throughput against instance count.
//
//   m8_bench <rom> [cyclesPerInstance] [render|norender]
//
// Reports aggregate emulated cycles per second as the instance count rises, so
// the point where the GPU stops scaling is visible rather than assumed. Every
// configuration runs the same ROM for the same number of cycles, which is what
// makes the numbers comparable across memory layouts.

#include "core/core.inc"
#include "gpu/instance_pool.h"
#include "gpu/vk_context.h"
#include "host/memory.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace gba;

namespace {

constexpr uint32_t kLocalSize = 64;
// Large enough that per-dispatch overhead does not dominate, small enough to
// stay well inside the macOS GPU watchdog. Swept separately below.
uint32_t gCyclesPerDispatch = 262144;

struct Row {
    uint32_t instances;
    double seconds;
    double aggregateMHz;
};

// Number of distinct code paths instances are steered down, for the divergence
// experiment. 0 disables seeding entirely.
uint32_t divergePaths = 0;

double runConfig(VkContext& ctx, const std::vector<uint32_t>& rom,
                 const std::vector<uint32_t>& bios, uint32_t instances, uint32_t cycles,
                 bool render) {
    InstancePool pool;
    pool.create(ctx, instances, uint32_t(rom.size()), render);
    uploadBuffer(ctx, pool.rom, rom.data(), rom.size() * 4);
    uploadBuffer(ctx, pool.bios, bios.data(), bios.size() * 4);

    std::vector<GbaState> states(instances);
    for (uint32_t i = 0; i < instances; ++i) {
        states[i] = GbaState{};
        host::hleBoot(states[i]);
        states[i].inst = i;
    }
    pool.uploadStates(ctx, states);

    // Seed each instance with (i % paths). The divergence ROM uses that as an
    // index into equal-cost code paths, so paths == 1 keeps a SIMD group in
    // lockstep and paths == 32 makes every one of its lanes differ.
    if (divergePaths > 0) {
        for (uint32_t i = 0; i < instances; ++i) {
            const uint32_t seed = i % divergePaths;
            uploadBuffer(ctx, pool.ewram, &seed, sizeof(seed),
                         VkDeviceSize(i) * EWRAM_WORDS * sizeof(uint32_t));
        }
    }

    ComputePipeline pipe;
    const char* shader = getenv("SHADER") ? getenv("SHADER") : "gba";
    pipe.create(ctx, std::string(SHADER_DIR) + "/" + shader + ".spv", uint32_t(pool.bindings().size()),
                sizeof(CorePush));
    pipe.bindBuffers(ctx, pool.bindings());

    // One untimed dispatch first: the first submission pays for pipeline
    // warm-up and page faults that have nothing to do with steady-state rate.
    CorePush warm{instances, uint32_t(rom.size()), 4096, render ? FLAG_RENDER : 0u};
    dispatchBlocking(ctx, pipe, (instances + kLocalSize - 1) / kLocalSize, &warm, sizeof(warm));

    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t done = 0; done < cycles;) {
        const uint32_t chunk = std::min(gCyclesPerDispatch, cycles - done);
        CorePush push{instances, uint32_t(rom.size()), chunk, render ? FLAG_RENDER : 0u};
        dispatchBlocking(ctx, pipe, (instances + kLocalSize - 1) / kLocalSize, &push, sizeof(push));
        done += chunk;
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    pipe.destroy(ctx);
    pool.destroy(ctx);
    return secs;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string romPath = argc > 1 ? argv[1] : "third_party/gba-tests/arm/arm.gba";
    const uint32_t cycles = argc > 2 ? uint32_t(std::atoi(argv[2])) : 2u * 1000 * 1000;
    const bool render = argc > 3 ? (std::string(argv[3]) == "render") : false;

    std::vector<uint32_t> rom;
    if (!host::loadBinary(romPath, rom)) {
        std::fprintf(stderr, "cannot read %s\n", romPath.c_str());
        return 2;
    }
    std::vector<uint32_t> bios;
    host::installBios(bios);

    // CPU baseline on one core, for the only comparison that matters.
    host::MemoryPool hostPool;
    hostPool.allocate(1, uint32_t(rom.size()), /*withFramebuffers=*/true);
    hostPool.bind();
    host::installBios(hostPool.bios);
    hostPool.io[REG_KEYINPUT >> 2] = 0x03FF;
    std::copy(rom.begin(), rom.end(), hostPool.rom.begin());
    g_flags = render ? FLAG_RENDER : 0u;

    GbaState cpu{};
    host::hleBoot(cpu);
    const auto c0 = std::chrono::steady_clock::now();
    step_cycles(cpu, cycles);
    const double cpuSecs = std::chrono::duration<double>(std::chrono::steady_clock::now() - c0)
                               .count();
    const double cpuMHz = double(cycles) / cpuSecs / 1e6;

    std::printf("%s, %u cycles/instance, rendering %s\n", romPath.c_str(), cycles,
                render ? "on" : "off");
    std::printf("CPU baseline: %.1f Mcycle/s on one core (%.1fx realtime)\n\n", cpuMHz,
                cpuMHz / 16.78);

    if (getenv("DIVERGE")) divergePaths = uint32_t(std::atoi(getenv("DIVERGE")));
    if (getenv("DISPATCH")) gCyclesPerDispatch = uint32_t(std::atoi(getenv("DISPATCH")));

    VkContext ctx;
    ctx.init(/*validation=*/false, /*debugPrintf=*/false);

    std::printf("%10s %9s %14s %13s %12s %14s\n", "instances", "seconds", "agg Mcycle/s",
                "MHz/instance", "x realtime", "x one CPU core");
    // ONLY=<n> measures a single instance count, for sweeps over some other
    // variable where the whole curve would be wasted work.
    std::vector<uint32_t> counts = {1, 32, 256, 1024, 2048, 4096, 8192};
    if (getenv("ONLY")) counts = {uint32_t(std::atoi(getenv("ONLY")))};
    for (uint32_t n : counts) {
        const double secs = runConfig(ctx, rom, bios, n, cycles, render);
        const double agg = double(cycles) * n / secs / 1e6;
        std::printf("%10u %9.2f %14.1f %13.2f %12.0f %14.1f\n", n, secs, agg, agg / n,
                    agg / 16.78, agg / cpuMHz);
        std::fflush(stdout);
    }

    ctx.destroy();
    return 0;
}
