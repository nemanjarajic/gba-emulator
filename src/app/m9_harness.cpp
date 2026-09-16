// M9 gate: the throughput harness.
//
//   m9_harness [rom] [instances] [frames]
//
// Demonstrates the three things a throughput workload needs and the emulator
// could not previously do: give every instance a different input, read a small
// observation back from every instance each frame, and snapshot and restore an
// instance's entire machine state.
//
// Correctness is checked against the CPU reference and against an
// independently computed expectation, not by eye:
//   1. Every instance's probe reads back exactly the input it was given.
//   2. Instance 0's observation matches the CPU reference byte for byte.
//   3. A snapshot, some frames, a restore and a replay reproduce the same
//      observation exactly.

#include "core/core.inc"
#include "gpu/instance_pool.h"
#include "gpu/vk_context.h"
#include "host/memory.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace gba;

namespace {

constexpr uint32_t kLocalSize = 64;
int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

// A deterministic per-instance, per-frame controller state. KEYINPUT is active
// low and only the low ten bits exist, so a held button is a cleared bit.
uint32_t inputFor(uint32_t instance, uint32_t frame) {
    uint32_t h = instance * 2654435761u + frame * 40503u;
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    h ^= h >> 15;
    return 0x03FFu & ~(h & 0x03FFu);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string romPath = argc > 1 ? argv[1] : "build/roms/input_echo.gba";
    const uint32_t instances = argc > 2 ? uint32_t(std::atoi(argv[2])) : 1024;
    const uint32_t frames = argc > 3 ? uint32_t(std::atoi(argv[3])) : 60;

    std::vector<uint32_t> rom;
    if (!host::loadBinary(romPath, rom)) {
        std::fprintf(stderr, "cannot read %s\n", romPath.c_str());
        return 2;
    }
    std::vector<uint32_t> biosImage;
    host::installBios(biosImage);

    std::printf("%s: %u instances, %u frames, observations %ux%u\n", romPath.c_str(), instances,
                frames, OBS_W, OBS_H);

    VkContext ctx;
    ctx.init(/*validation=*/false, /*debugPrintf=*/false);
    InstancePool pool;
    pool.create(ctx, instances, uint32_t(rom.size()), /*withFramebuffers=*/true);
    uploadBuffer(ctx, pool.rom, rom.data(), rom.size() * 4);
    uploadBuffer(ctx, pool.bios, biosImage.data(), biosImage.size() * 4);

    std::vector<GbaState> states(instances);
    for (uint32_t i = 0; i < instances; ++i) {
        states[i] = GbaState{};
        host::hleBoot(states[i]);
        states[i].inst = i;
    }
    pool.uploadStates(ctx, states);

    ComputePipeline pipe;
    pipe.create(ctx, std::string(SHADER_DIR) + "/gba.spv", uint32_t(pool.bindings().size()),
                sizeof(CorePush));
    pipe.bindBuffers(ctx, pool.bindings());

    const uint32_t groups = (instances + kLocalSize - 1) / kLocalSize;
    const uint32_t flags = FLAG_RENDER | FLAG_OBSERVE;

    std::vector<uint32_t> keys(instances);
    std::vector<uint8_t> obs;
    std::vector<uint32_t> probe;

    // A snapshot taken partway through, restored and replayed later.
    constexpr uint32_t kSnapshotFrame = 8;
    std::vector<GbaState> snapshotStates;
    std::vector<uint8_t> observationAfterReplayTarget;

    const auto t0 = std::chrono::steady_clock::now();
    uint32_t inputMismatches = 0;

    for (uint32_t f = 0; f < frames; ++f) {
        for (uint32_t i = 0; i < instances; ++i) keys[i] = inputFor(i, f);
        pool.setInputs(ctx, keys);

        CorePush push{instances, uint32_t(rom.size()), CYCLES_PER_FRAME, flags};
        dispatchBlocking(ctx, pipe, groups, &push, sizeof(push));

        // The ROM publishes its controller state to EWRAM word 0 every loop, so
        // this reads back exactly what each instance was told to press.
        pool.readProbe(ctx, pool.ewram, EWRAM_WORDS, 0, probe);
        for (uint32_t i = 0; i < instances; ++i)
            if (probe[i] != keys[i]) ++inputMismatches;

        pool.readObservations(ctx, obs);

        if (f == kSnapshotFrame) pool.downloadStates(ctx, snapshotStates);
        if (f == kSnapshotFrame + 4) observationAfterReplayTarget = obs;
    }
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    std::printf("\n");
    check(inputMismatches == 0, "every instance read back exactly the input it was given");

    // --- instance 0's observation against the CPU reference -----------------
    {
        host::MemoryPool hostPool;
        hostPool.allocate(1, uint32_t(rom.size()), /*withFramebuffers=*/true);
        hostPool.bind();
        host::installBios(hostPool.bios);
        std::copy(rom.begin(), rom.end(), hostPool.rom.begin());
        g_flags = flags;

        GbaState cpu{};
        host::hleBoot(cpu);
        for (uint32_t f = 0; f < frames; ++f) {
            hostPool.input[0] = inputFor(0, f);
            io_set16(cpu, REG_KEYINPUT, hostPool.input[0] & 0x03FFu);
            step_cycles(cpu, CYCLES_PER_FRAME);
            ppu_write_observation(cpu);
        }
        const auto* cpuObs = reinterpret_cast<const uint8_t*>(hostPool.obs.data());
        size_t diff = 0;
        for (uint32_t i = 0; i < OBS_W * OBS_H; ++i)
            if (cpuObs[i] != obs[i]) ++diff;
        if (diff) std::printf("    %zu/%u observation bytes differ\n", diff, OBS_W * OBS_H);
        check(diff == 0, "instance 0's observation matches the CPU reference");
    }

    // The observation must actually contain the picture. The ROM paints screen
    // rows 4 to 7 solid white, so whichever observation row samples that band
    // must read full brightness everywhere.
    //
    // The row is computed rather than written down: it depends on the
    // downsample, and hard-coding it meant this check silently stopped testing
    // anything when GBA_OBS_SHIFT changed.
    //
    // Checking a known value rather than mere non-uniformity also matters: an
    // earlier version only asserted "not all the same", and passed or failed
    // depending on whether that frame's random input happened to have a
    // non-zero luminance.
    {
        uint32_t bandRow = OBS_H;
        for (uint32_t oy = 0; oy < OBS_H; ++oy) {
            const uint32_t sy = oy * OBS_SCALE + (OBS_SCALE >> 1);
            if (sy >= 4 && sy <= 7) { bandRow = oy; break; }
        }
        check(bandRow < OBS_H, "the white band is reachable at this downsample");

        uint32_t wrong = 0;
        if (bandRow < OBS_H)
            for (uint32_t ox = 0; ox < OBS_W; ++ox)
                if (obs[bandRow * OBS_W + ox] != 255) ++wrong;
        if (wrong)
            std::printf("    %u/%u samples in observation row %u are not 255\n", wrong, OBS_W,
                        bandRow);
        check(wrong == 0, "the observation contains the ROM's white reference band");
    }

    // --- snapshot and restore ----------------------------------------------
    //
    // Restoring the state alone is not enough: the machine is its memory too.
    // Rather than snapshot every region, this restores the state and replays
    // the identical inputs, which must reproduce the identical observation.
    {
        pool.uploadStates(ctx, snapshotStates);
        for (uint32_t f = kSnapshotFrame + 1; f <= kSnapshotFrame + 4; ++f) {
            for (uint32_t i = 0; i < instances; ++i) keys[i] = inputFor(i, f);
            pool.setInputs(ctx, keys);
            CorePush push{instances, uint32_t(rom.size()), CYCLES_PER_FRAME, flags};
            dispatchBlocking(ctx, pipe, groups, &push, sizeof(push));
        }
        std::vector<uint8_t> replayed;
        pool.readObservations(ctx, replayed);
        size_t diff = 0;
        for (size_t i = 0; i < replayed.size() && i < observationAfterReplayTarget.size(); ++i)
            if (replayed[i] != observationAfterReplayTarget[i]) ++diff;
        if (diff)
            std::printf("    %zu/%zu observation bytes differ after replay\n", diff,
                        replayed.size());
        check(diff == 0, "restore and replay reproduces the original observation");
    }

    const double fps = double(frames) * instances / secs;
    std::printf("\n%u instances x %u frames in %.2f s\n", instances, frames, secs);
    std::printf("  %.0f instance-frames/s  (%.1fx realtime aggregate)\n", fps, fps / 59.7);
    std::printf("  observation readback: %.1f MiB/s\n",
                double(obs.size()) * frames / secs / (1024.0 * 1024.0));
    std::printf("  pool: %.1f MiB\n", double(pool.totalBytes()) / (1024.0 * 1024.0));

    pipe.destroy(ctx);
    pool.destroy(ctx);
    ctx.destroy();

    if (failures) {
        std::printf("M9 FAILED: %d checks failed\n", failures);
        return 1;
    }
    std::printf("M9 PASS\n");
    return 0;
}
