// Runs the CPU reference and the GPU core from the same saved state, one frame
// at a time, and reports the first frame on which they disagree.
//
//   state_parity <rom> <state> [frames]
//
// The M4 gate proves the two cores agree from a cold boot over a short run.
// This one starts wherever a reset point was captured and keeps going, which is
// how a divergence that only appears minutes into a game gets found: Pokemon
// Emerald halted on the GPU at frame 2688 from one such state while the CPU
// core carried on.

#include "core/core.inc"
#include "gpu/instance_pool.h"
#include "gpu/vk_context.h"
#include "host/disasm.h"
#include "host/memory.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace gba;

namespace {

constexpr uint32_t kLocalSize = 64;

bool readWords(std::ifstream& f, std::vector<uint32_t>& v) {
    uint32_t n = 0;
    f.read(reinterpret_cast<char*>(&n), 4);
    if (!f || n != v.size()) return false;
    f.read(reinterpret_cast<char*>(v.data()), std::streamsize(n) * 4);
    return bool(f);
}

// Names for the fields of GbaState, so a mismatch says what differs.
const char* stateFieldName(size_t word) {
    static const struct { size_t off, len; const char* name; } kFields[] = {
        {offsetof(GbaState, r) / 4, 16, "r"},
        {offsetof(GbaState, cpsr) / 4, 1, "cpsr"},
        {offsetof(GbaState, spsr) / 4, NUM_BANKS, "spsr"},
        {offsetof(GbaState, cycles) / 4, 1, "cycles"},
        {offsetof(GbaState, halted) / 4, 1, "halted"},
        {offsetof(GbaState, scanline) / 4, 1, "scanline"},
        {offsetof(GbaState, line_cycle) / 4, 1, "line_cycle"},
        {offsetof(GbaState, irq_ready) / 4, 1, "irq_ready"},
        {offsetof(GbaState, timer_active) / 4, 1, "timer_active"},
        {offsetof(GbaState, dma_enabled) / 4, 1, "dma_enabled"},
    };
    for (const auto& f : kFields)
        if (word >= f.off && word < f.off + f.len) return f.name;
    return "(other)";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: state_parity <rom> <state> [frames]\n");
        return 2;
    }
    const std::string romPath = argv[1], statePath = argv[2];
    const uint32_t frames = argc > 3 ? uint32_t(std::atoi(argv[3])) : 4000;

    std::vector<uint32_t> rom;
    if (!host::loadBinary(romPath, rom)) {
        std::fprintf(stderr, "cannot read %s\n", romPath.c_str());
        return 2;
    }

    // ---- the state, into the CPU reference ---------------------------------
    host::MemoryPool hostPool;
    hostPool.allocate(1, uint32_t(rom.size()), /*withFramebuffers=*/true);
    hostPool.bind();
    host::installBios(hostPool.bios);
    std::copy(rom.begin(), rom.end(), hostPool.rom.begin());

    GbaState cpu{};
    std::ifstream f(statePath, std::ios::binary);
    f.read(reinterpret_cast<char*>(&cpu), sizeof(GbaState));
    if (!f || !readWords(f, hostPool.ewram) || !readWords(f, hostPool.iwram) ||
        !readWords(f, hostPool.vram) || !readWords(f, hostPool.pram) ||
        !readWords(f, hostPool.oam) || !readWords(f, hostPool.io) || !readWords(f, hostPool.sram)) {
        std::fprintf(stderr, "cannot read %s\n", statePath.c_str());
        return 2;
    }
    cpu.inst = 0;
    g_flags = FLAG_RENDER;

    // ---- the same state, into one GPU instance -----------------------------
    VkContext ctx;
    ctx.init(/*validation=*/false, /*debugPrintf=*/false);
    InstancePool pool;
    pool.create(ctx, 1, uint32_t(rom.size()), /*withFramebuffers=*/true);
    uploadBuffer(ctx, pool.rom, rom.data(), rom.size() * 4);
    uploadBuffer(ctx, pool.bios, hostPool.bios.data(), hostPool.bios.size() * 4);

    InstancePool::Snapshot snap;
    snap.state = cpu;
    snap.ewram = hostPool.ewram;
    snap.iwram = hostPool.iwram;
    snap.vram = hostPool.vram;
    snap.pram = hostPool.pram;
    snap.oam = hostPool.oam;
    snap.io = hostPool.io;
    snap.sram = hostPool.sram;
    pool.restoreInstances(ctx, {0}, snap);

    ComputePipeline pipe;
    pipe.create(ctx, std::string(SHADER_DIR) + "/gba.spv", uint32_t(pool.bindings().size()),
                sizeof(CorePush));
    pipe.bindBuffers(ctx, pool.bindings());

    std::printf("%s from %s: %u frames on both cores\n", romPath.c_str(), statePath.c_str(), frames);

    std::vector<GbaState> gpuStates;
    const auto* cpuWords = reinterpret_cast<const uint32_t*>(&cpu);

    for (uint32_t frame = 0; frame < frames; ++frame) {
        CorePush push{1, uint32_t(rom.size()), CYCLES_PER_FRAME, pool.baseFlags() | FLAG_RENDER};
        dispatchBlocking(ctx, pipe, (1 + kLocalSize - 1) / kLocalSize, &push, sizeof(push));
        step_cycles(cpu, CYCLES_PER_FRAME);

        pool.downloadStates(ctx, gpuStates);
        const auto* gpuWords = reinterpret_cast<const uint32_t*>(&gpuStates[0]);
        for (size_t w = 0; w < sizeof(GbaState) / 4; ++w) {
            if (cpuWords[w] == gpuWords[w]) continue;
            std::printf("\nframe %u: GbaState word %zu (%s) differs: CPU %08X, GPU %08X\n", frame, w,
                        stateFieldName(w), cpuWords[w], gpuWords[w]);
            std::printf("  CPU pc=%08X cpsr=%08X halted=%u cycles=%u scanline=%u\n", cpu.r[15],
                        cpu.cpsr, cpu.halted, cpu.cycles, cpu.scanline);
            std::printf("  GPU pc=%08X cpsr=%08X halted=%u cycles=%u scanline=%u\n",
                        gpuStates[0].r[15], gpuStates[0].cpsr, gpuStates[0].halted,
                        gpuStates[0].cycles, gpuStates[0].scanline);
            const bool thumb = (cpu.cpsr & CPSR_T) != 0;
            std::printf("  CPU is at %s\n",
                        (thumb ? host::disasmThumb(uint16_t(bus_read16(cpu, cpu.r[15])), cpu.r[15])
                               : host::disasmArm(bus_read32(cpu, cpu.r[15]), cpu.r[15])).c_str());
            pipe.destroy(ctx);
            pool.destroy(ctx);
            ctx.destroy();
            return 1;
        }
        if (frame % 200 == 0) {
            std::printf("  frame %4u ok (pc=%08X halted=%u)\n", frame, cpu.r[15], cpu.halted);
            std::fflush(stdout);
        }
    }

    std::printf("\nPARITY PASS: the cores agree for %u frames\n", frames);
    pipe.destroy(ctx);
    pool.destroy(ctx);
    ctx.destroy();
    return 0;
}
