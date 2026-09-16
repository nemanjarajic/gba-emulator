// M1 parity gate: the same bus source, compiled as C++ and as GLSL, must
// produce identical results.
//
// This is the M4 differential harness in miniature. It now covers the ARM and
// Thumb interpreters as well as the bus: each instance executes 256 cycles of
// pseudorandom cartridge contents and the full machine state is hashed, so any
// instruction the two compilers disagree about shows up as a mismatch.

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
//
// Pseudorandom rather than a simple ramp: the cartridge is executed as code by
// cpu_selftest, and a ramp would decode to long runs of the same condition code
// and exercise almost nothing. Well-mixed words hit the whole decoder.
uint32_t romWord(uint32_t i) {
    uint32_t x = i * 2654435761u + 0x9E3779B9u;
    x ^= x >> 16; x *= 0x7FEB352Du;
    x ^= x >> 15; x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}
uint32_t biosWord(uint32_t i) { return 0xB1050000u + i; }

}  // namespace

int main() {
    // ---- GPU run ----------------------------------------------------------
    VkContext ctx;
    ctx.init(/*validation=*/true, /*debugPrintf=*/false);

    InstancePool pool;
    pool.create(ctx, kInstances, kRomWords, /*withFramebuffers=*/true);

    std::vector<uint32_t> romImage(kRomWords);
    for (uint32_t i = 0; i < kRomWords; ++i) romImage[i] = romWord(i);
    uploadBuffer(ctx, pool.rom, romImage.data(), romImage.size() * 4);

    std::vector<uint32_t> biosImage(BIOS_WORDS);
    for (uint32_t i = 0; i < BIOS_WORDS; ++i) biosImage[i] = biosWord(i);
    uploadBuffer(ctx, pool.bios, biosImage.data(), biosImage.size() * 4);

    ComputePipeline pipe;
    pipe.create(ctx, std::string(SHADER_DIR) + "/core_compile_test.spv",
                /*numBuffers=*/uint32_t(pool.bindings().size()), sizeof(CorePush));
    pipe.bindBuffers(ctx, pool.bindings());

    CorePush push{kInstances, kRomWords, 0, 0};
    dispatchBlocking(ctx, pipe, (kInstances + kLocalSize - 1) / kLocalSize, &push, sizeof(push));

    std::vector<uint32_t> fbImage(size_t(kInstances) * FB_WORDS);
    downloadBuffer(ctx, pool.fb, fbImage.data(), fbImage.size() * 4);
    std::vector<uint32_t> gpuResult(kInstances);
    for (uint32_t i = 0; i < kInstances; ++i) gpuResult[i] = fbImage[size_t(i) * FB_WORDS];

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
        uint32_t r = bus_selftest(st);
        r = r * 33u + cpu_selftest(st, 256u);
        cpuResult[i] = r;
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
