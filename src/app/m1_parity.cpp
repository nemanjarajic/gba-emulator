// M1 parity gate: the same bus source, compiled as C++ and as GLSL, must
// produce identical results.
//
// This is the smallest useful version of the M4 differential harness. The whole
// project depends on one source tree compiling both ways and *behaving* the
// same; proving that at M1 costs little, whereas discovering it is false at M4
// would invalidate the architecture after the interpreter is already written.

#include "core/selftest.inc"
#include "gpu/instance_pool.h"
#include "gpu/vk_context.h"
#include "host/memory.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace gba;

namespace {

constexpr uint32_t kInstances = 256;
constexpr uint32_t kLocalSize = 64;  // must match core_compile_test.comp
constexpr uint32_t kRomWords = 0x1000;

// Identical synthetic cartridge and BIOS contents on both sides.
uint32_t romWord(uint32_t i) { return 0xA0000000u + i * 0x01010101u; }
uint32_t biosWord(uint32_t i) { return 0xB1050000u + i; }

}  // namespace

int main() {
    // ---- GPU run ----------------------------------------------------------
    VkContext ctx;
    ctx.init(/*validation=*/true, /*debugPrintf=*/false);

    InstancePool pool;
    pool.create(ctx, kInstances, kRomWords, /*withFramebuffers=*/true);

    auto* gpuRom = static_cast<uint32_t*>(pool.rom.mapped);
    for (uint32_t i = 0; i < kRomWords; ++i) gpuRom[i] = romWord(i);
    auto* gpuBios = static_cast<uint32_t*>(pool.bios.mapped);
    for (uint32_t i = 0; i < BIOS_WORDS; ++i) gpuBios[i] = biosWord(i);

    ComputePipeline pipe;
    pipe.create(ctx, std::string(SHADER_DIR) + "/core_compile_test.spv",
                /*numBuffers=*/uint32_t(pool.bindings().size()), sizeof(CorePush));
    pipe.bindBuffers(ctx, pool.bindings());

    CorePush push{kInstances, kRomWords, 0, 0};
    dispatchBlocking(ctx, pipe, (kInstances + kLocalSize - 1) / kLocalSize, &push, sizeof(push));

    const auto* gpuFb = static_cast<const uint32_t*>(pool.fb.mapped);
    std::vector<uint32_t> gpuResult(kInstances);
    for (uint32_t i = 0; i < kInstances; ++i) gpuResult[i] = gpuFb[size_t(i) * FB_WORDS];

    std::printf("GPU: %u instances, %.1f MiB of state\n", kInstances,
                double(pool.totalBytes()) / (1024.0 * 1024.0));

    // ---- CPU run ----------------------------------------------------------
    host::MemoryPool hostPool;
    hostPool.allocate(kInstances, kRomWords, /*withFramebuffers=*/true);
    hostPool.bind();
    for (uint32_t i = 0; i < kRomWords; ++i) hostPool.rom[i] = romWord(i);
    for (uint32_t i = 0; i < BIOS_WORDS; ++i) hostPool.bios[i] = biosWord(i);

    std::vector<uint32_t> cpuResult(kInstances);
    for (uint32_t i = 0; i < kInstances; ++i) {
        GbaState st{};
        selftest_init(st, i);
        cpuResult[i] = bus_selftest(st);
    }

    // ---- compare ----------------------------------------------------------
    uint32_t mismatches = 0;
    for (uint32_t i = 0; i < kInstances; ++i) {
        if (cpuResult[i] == gpuResult[i]) continue;
        if (mismatches < 8)
            std::printf("  instance %4u: CPU 0x%08X  GPU 0x%08X\n", i, cpuResult[i], gpuResult[i]);
        ++mismatches;
    }

    // A result of zero everywhere would make the comparison pass vacuously.
    bool allZero = true;
    for (uint32_t i = 0; i < kInstances; ++i)
        if (cpuResult[i] != 0u) { allZero = false; break; }

    pipe.destroy(ctx);
    pool.destroy(ctx);
    ctx.destroy();

    if (mismatches) {
        std::printf("M1 PARITY FAILED: %u/%u instances disagree\n", mismatches, kInstances);
        return 1;
    }
    if (allZero) {
        std::printf("M1 PARITY INCONCLUSIVE: every result is zero\n");
        return 1;
    }
    std::printf("M1 PARITY PASS: %u instances agree (e.g. inst 0 = 0x%08X, inst 255 = 0x%08X)\n",
                kInstances, cpuResult[0], cpuResult[kInstances - 1]);
    return 0;
}
