// Preflight check for a GPU this emulator has not run on before.
//
// Reports what the device offers, how many instances will fit, which memory
// path the buffers landed in, and whether the two measurements that came out
// counter-intuitively on Apple Silicon go the same way here:
//
//   * the register cliff -- adding four words to GbaState cost 2.9x on an M4,
//     and that ceiling will sit somewhere different on another architecture
//   * the memory layout -- the interleaved layout lost by 21-45% on an M4,
//     which may well reverse on a discrete GPU's cache hierarchy
//
// Run this first. Everything it prints is measured on the machine it runs on.

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

double benchmark(VkContext& ctx, const std::string& shader, uint32_t instances, uint32_t cycles,
                 const std::vector<uint32_t>& rom, const std::vector<uint32_t>& bios) {
    InstancePool pool;
    pool.create(ctx, instances, uint32_t(rom.size()), /*withFramebuffers=*/false,
                /*withSave=*/true);
    uploadBuffer(ctx, pool.rom, rom.data(), rom.size() * 4);
    uploadBuffer(ctx, pool.bios, bios.data(), bios.size() * 4);

    std::vector<GbaState> states(instances);
    for (uint32_t i = 0; i < instances; ++i) {
        states[i] = GbaState{};
        host::hleBoot(states[i]);
        states[i].inst = i;
    }
    pool.uploadStates(ctx, states);

    ComputePipeline pipe;
    pipe.create(ctx, std::string(SHADER_DIR) + "/" + shader + ".spv",
                uint32_t(pool.bindings().size()), sizeof(CorePush));
    pipe.bindBuffers(ctx, pool.bindings());

    const uint32_t groups = (instances + kLocalSize - 1) / kLocalSize;
    CorePush warm{instances, uint32_t(rom.size()), 4096, pool.baseFlags()};
    dispatchBlocking(ctx, pipe, groups, &warm, sizeof(warm));

    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t done = 0; done < cycles;) {
        const uint32_t chunk = std::min(262144u, cycles - done);
        CorePush push{instances, uint32_t(rom.size()), chunk, pool.baseFlags()};
        dispatchBlocking(ctx, pipe, groups, &push, sizeof(push));
        done += chunk;
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    pipe.destroy(ctx);
    pool.destroy(ctx);
    return double(cycles) * instances / secs / 1e6;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string romPath = argc > 1 ? argv[1] : "third_party/gba-tests/arm/arm.gba";
    const uint32_t benchInstances = argc > 2 ? uint32_t(std::atoi(argv[2])) : 4096;

    VkContext ctx;
    ctx.init(/*validation=*/false, /*debugPrintf=*/false);

    const auto& lim = ctx.props.limits;
    std::printf("device\n");
    std::printf("  %-34s %s\n", "name", ctx.props.deviceName);
    std::printf("  %-34s %u.%u.%u\n", "Vulkan", VK_VERSION_MAJOR(ctx.props.apiVersion),
                VK_VERSION_MINOR(ctx.props.apiVersion), VK_VERSION_PATCH(ctx.props.apiVersion));
    std::printf("  %-34s %u\n", "subgroup size (instances/group)", ctx.subgroupSize);

    VkDeviceSize heap = 0;
    for (uint32_t i = 0; i < ctx.memProps.memoryHeapCount; ++i)
        if (ctx.memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            heap = std::max(heap, ctx.memProps.memoryHeaps[i].size);

    std::printf("\nlimits that matter\n");
    std::printf("  %-34s %.2f GiB\n", "device-local heap", double(heap) / (1u << 30));
    std::printf("  %-34s %.2f GiB\n", "maxStorageBufferRange",
                double(lim.maxStorageBufferRange) / (1u << 30));
    std::printf("  %-34s %u  (need 13)\n", "maxPerStageDescriptorStorageBuffers",
                lim.maxPerStageDescriptorStorageBuffers);
    std::printf("  %-34s %u  (need 64)\n", "maxComputeWorkGroupInvocations",
                lim.maxComputeWorkGroupInvocations);
    std::printf("  %-34s %u\n", "maxComputeWorkGroupCount[0]", lim.maxComputeWorkGroupCount[0]);

    std::printf("\nhow many instances fit\n");
    struct Config { const char* name; bool fb, save; };
    const Config configs[] = {
        {"headless, with save", false, true},
        {"headless, no save", false, false},
        {"observations + save", true, true},
        {"observations, no save", true, false},
    };
    for (const Config& c : configs) {
        const uint64_t per = InstancePool::bytesPerInstance(c.fb, c.save);
        // The same 15% margin the pool leaves for the driver and the display.
        uint64_t byHeap = uint64_t(double(heap) * 0.85 / double(per));
        uint64_t byRange = lim.maxStorageBufferRange / (uint64_t(EWRAM_WORDS) * 4);
        std::printf("  %-24s %6.1f KB each  ->  %6llu  (heap %llu, buffer limit %llu)\n", c.name,
                    double(per) / 1024.0, (unsigned long long)std::min(byHeap, byRange),
                    (unsigned long long)byHeap, (unsigned long long)byRange);
    }

    // --- measurements -------------------------------------------------------
    std::vector<uint32_t> rom;
    if (!host::loadBinary(romPath, rom)) {
        std::printf("\n(skipping measurements: cannot read %s)\n", romPath.c_str());
        ctx.destroy();
        return 0;
    }
    std::vector<uint32_t> bios;
    host::installBios(bios);

    if (!InstancePool::fits(ctx, benchInstances, uint32_t(rom.size()), false, true, false)) {
        std::printf("\n(skipping measurements: %u instances do not fit)\n", benchInstances);
        ctx.destroy();
        return 1;
    }

    std::printf("\nmeasurements at %u instances (%s)\n", benchInstances, romPath.c_str());
    const uint32_t cycles = 1000000;
    const double base = benchmark(ctx, "gba", benchInstances, cycles, rom, bios);
    std::printf("  %-34s %8.1f Mcycle/s\n", "baseline", base);

    {
        InstancePool probe;
        probe.create(ctx, 8, uint32_t(rom.size()), false, true);
        std::printf("  %-34s %s\n", "buffer memory",
                    probe.ewram.mapped ? "host-mapped device-local (fast path)"
                                       : "device-local + staging (portable path)");
        probe.destroy(ctx);
    }

    const double pad = benchmark(ctx, "gba_bench_pad4", benchInstances, cycles, rom, bios);
    std::printf("  %-34s %8.1f Mcycle/s  (%.2fx baseline)\n", "GbaState + 4 words", pad,
                pad / base);
    if (pad < base * 0.75)
        std::printf("      -> this GPU has the register cliff too. Do not add fields to\n"
                    "         GbaState; see the note in src/core/state.h.\n");
    else
        std::printf("      -> no cliff here: this GPU has register headroom that Apple\n"
                    "         Silicon did not, so GbaState could afford to grow.\n");

    const double soa = benchmark(ctx, "gba_soa", benchInstances, cycles, rom, bios);
    std::printf("  %-34s %8.1f Mcycle/s  (%.2fx baseline)\n", "interleaved memory layout", soa,
                soa / base);
    if (soa > base * 1.05)
        std::printf("      -> interleaving WINS here, unlike on Apple Silicon. Build the\n"
                    "         emulator with GBA_SOA and re-run the gates.\n");
    else
        std::printf("      -> interleaving loses here too; keep the default layout.\n");

    ctx.destroy();
    return 0;
}
