// M0 gate: a trivial compute shader squares 1024 integers and the host reads
// back the correct result.
//
// Also prints the device limits the whole design depends on, so that any
// assumption in the plan can be re-checked against the actual machine rather
// than trusted from a doc.

#include "gpu/vk_context.h"

#include <cinttypes>
#include <cstdio>
#include <vector>

using namespace gba;

namespace {

constexpr uint32_t kN = 1024;
constexpr uint32_t kLocalSize = 64;  // must match square.comp

struct Push {
    uint32_t n;
    uint32_t traceLane;
};

void reportLimits(const VkContext& ctx) {
    const auto& l = ctx.props.limits;
    std::printf("device                          : %s (MoltenVK)\n", ctx.props.deviceName);
    std::printf("subgroupSize                    : %u  <- GBA instances per SIMD group\n",
                ctx.subgroupSize);
    std::printf("maxComputeWorkGroupInvocations  : %u\n", l.maxComputeWorkGroupInvocations);
    std::printf("maxComputeSharedMemorySize      : %u bytes\n", l.maxComputeSharedMemorySize);
    std::printf("maxPerStageDescriptorStorageBufs: %u  <- we need ~10\n",
                l.maxPerStageDescriptorStorageBuffers);
    std::printf("maxStorageBufferRange           : %.2f GiB\n",
                double(l.maxStorageBufferRange) / (1024.0 * 1024.0 * 1024.0));

    VkDeviceSize heap = 0;
    for (uint32_t i = 0; i < ctx.memProps.memoryHeapCount; ++i)
        if (ctx.memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            heap = ctx.memProps.memoryHeaps[i].size > heap ? ctx.memProps.memoryHeaps[i].size
                                                           : heap;
    std::printf("device-local heap               : %.2f GiB\n",
                double(heap) / (1024.0 * 1024.0 * 1024.0));

    // Per-instance GBA state, from the plan's memory budget. ROM and BIOS are
    // shared across instances and so are excluded here.
    constexpr double kPerInstanceKiB = 256 + 32 + 96 + 1 + 1 + 1 + 128;  // ~515 KiB
    std::printf("\nper-instance state              : %.0f KiB (EWRAM+IWRAM+VRAM+PRAM+OAM+IO+save)\n",
                kPerInstanceKiB);
    for (uint32_t n : {1024u, 4096u, 8192u, 16384u}) {
        const double gib = kPerInstanceKiB * n / (1024.0 * 1024.0);
        std::printf("  %6u instances              : %6.2f GiB%s\n", n, gib,
                    gib * 1024.0 * 1024.0 * 1024.0 > double(heap) * 0.66 ? "   (over budget)" : "");
    }
    std::printf("\n");
}

}  // namespace

int main() {
    VkContext ctx;
    ctx.init(/*validation=*/true, /*debugPrintf=*/true);
    reportLimits(ctx);

    Buffer data = createStorageBuffer(ctx, kN * sizeof(uint32_t));
    auto* host = static_cast<uint32_t*>(data.mapped);
    for (uint32_t i = 0; i < kN; ++i) host[i] = i;

    ComputePipeline pipe;
    pipe.create(ctx, std::string(SHADER_DIR) + "/square.spv", /*numBuffers=*/1, sizeof(Push));
    pipe.bindBuffers(ctx, {&data});

    Push push{kN, 37};
    dispatchBlocking(ctx, pipe, (kN + kLocalSize - 1) / kLocalSize, &push, sizeof(push));

    uint32_t bad = 0;
    for (uint32_t i = 0; i < kN; ++i) {
        const uint32_t want = i * i;
        if (host[i] != want) {
            if (bad < 8)
                std::fprintf(stderr, "mismatch at %u: got %u want %u\n", i, host[i], want);
            ++bad;
        }
    }

    pipe.destroy(ctx);
    destroyBuffer(ctx, data);
    ctx.destroy();

    if (bad) {
        std::printf("M0 FAILED: %u/%u mismatches\n", bad, kN);
        return 1;
    }
    std::printf("M0 PASS: %u integers squared on the GPU\n", kN);
    return 0;
}
