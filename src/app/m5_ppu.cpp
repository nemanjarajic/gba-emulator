// M5 gate: first pixels.
//
// The plan called for "a mode-3 test ROM", but jsmolka's ppu/ ROMs all set
// BG0CNT -- they are tiled-mode tests, and belong to M6. There is no bitmap
// mode ROM in the suite, so this builds a small one.
//
// It is real ARM machine code executed by the emulator, not host-poked memory:
// the point of the milestone is that a ROM draws something, and driving VRAM
// from the host would skip the part being tested.
//
// Checks, in increasing strength:
//   1. The CPU reference renders a plausible image (not blank, not uniform).
//   2. The GPU framebuffer matches the CPU's pixel for pixel.
//   3. Modes 4 and 5 agree between CPU and GPU as well.

#include "core/core.inc"
#include "gpu/instance_pool.h"
#include "gpu/vk_context.h"
#include "host/memory.h"
#include "host/png.h"

#include <cstdio>
#include <string>
#include <vector>

using namespace gba;

namespace {

constexpr uint32_t kLocalSize = 64;
constexpr uint32_t kInstances = 64;
int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-44s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

// Hand-assembled bitmap-mode test ROMs.
//
// Each writes a pattern whose value is derived from the pixel index, so a
// single misplaced pixel is visible -- unlike a flat fill, which would hide
// an addressing bug. Colour is BGR555: low 5 bits red, next 5 green, next 5
// blue, so an index ramp gives fine vertical stripes over a slow gradient.

// Mode 3: 240x160, one 16-bit colour per pixel, written straight to VRAM.
std::vector<uint32_t> buildMode3Rom() {
    return {
        0xE3A00404u,  // MOV  r0, #0x04000000        I/O base
        0xE3A01B01u,  // MOV  r1, #0x400             DISPCNT: BG2 enable
        0xE3811003u,  // ORR  r1, r1, #3             ...and mode 3
        0xE5801000u,  // STR  r1, [r0]
        0xE3A02406u,  // MOV  r2, #0x06000000        VRAM base
        0xE3A03000u,  // MOV  r3, #0                 pixel index
        0xE1C230B0u,  // loop: STRH r3, [r2]         colour = index
        0xE2822002u,  // ADD  r2, r2, #2
        0xE2833001u,  // ADD  r3, r3, #1
        0xE3530C96u,  // CMP  r3, #0x9600            240*160 = 38400
        0x1AFFFFFAu,  // BNE  loop
        0xEAFFFFFEu,  // B    .
    };
}

// Mode 4: 240x160 of palette indices, with entry[i] = i so the rendered colour
// still equals the index.
//
// Pixels are written a HALFWORD at a time, two at once. Writing them
// individually with STRB does not work on hardware: an 8-bit write to VRAM is
// doubled across the containing halfword, so each store would also overwrite
// its neighbour. The first version of this ROM did exactly that and produced
// 128 colours instead of 256 -- the emulator was right and the ROM was wrong.
// It is the classic mode-4 trap, and writing pairs is what real code does.
std::vector<uint32_t> buildMode4Rom() {
    return {
        0xE3A00404u,  // MOV  r0, #0x04000000
        0xE3A01B01u,  // MOV  r1, #0x400
        0xE3811004u,  // ORR  r1, r1, #4             mode 4
        0xE5801000u,  // STR  r1, [r0]
        0xE3A02405u,  // MOV  r2, #0x05000000        palette RAM
        0xE3A03000u,  // MOV  r3, #0
        0xE1C230B0u,  // pal: STRH r3, [r2]          palette[i] = i
        0xE2822002u,  // ADD  r2, r2, #2
        0xE2833001u,  // ADD  r3, r3, #1
        0xE3530C01u,  // CMP  r3, #0x100             256 entries
        0x1AFFFFFAu,  // BNE  pal
        0xE3A02406u,  // MOV  r2, #0x06000000        VRAM base
        0xE3A03000u,  // MOV  r3, #0                 pair index
        0xE20340FFu,  // px:  AND  r4, r3, #0xFF
        0xE1844404u,  // ORR  r4, r4, r4, LSL #8     same index in both bytes
        0xE1C240B0u,  // STRH r4, [r2]               two pixels at once
        0xE2822002u,  // ADD  r2, r2, #2
        0xE2833001u,  // ADD  r3, r3, #1
        0xE3530C4Bu,  // CMP  r3, #0x4B00            38400/2 = 19200 pairs
        0x1AFFFFF8u,  // BNE  px
        0xEAFFFFFEu,  // B    .
    };
}

// Mode 5: 160x128 of 16-bit colour. The area outside that shows the backdrop,
// which stays black because this ROM never writes the palette.
std::vector<uint32_t> buildMode5Rom() {
    return {
        0xE3A00404u,  // MOV  r0, #0x04000000
        0xE3A01B01u,  // MOV  r1, #0x400
        0xE3811005u,  // ORR  r1, r1, #5             mode 5
        0xE5801000u,  // STR  r1, [r0]
        0xE3A02406u,  // MOV  r2, #0x06000000
        0xE3A03000u,  // MOV  r3, #0
        0xE1C230B0u,  // loop: STRH r3, [r2]
        0xE2822002u,  // ADD  r2, r2, #2
        0xE2833001u,  // ADD  r3, r3, #1
        0xE3530C50u,  // CMP  r3, #0x5000            160*128 = 20480
        0x1AFFFFFAu,  // BNE  loop
        0xEAFFFFFEu,  // B    .
    };
}

// What each mode should put at pixel (x, y), given the patterns above.
uint32_t expectedPixel(uint32_t mode, uint32_t x, uint32_t y) {
    const uint32_t i = y * SCREEN_W + x;
    if (mode == 3) return i & 0xFFFF;
    if (mode == 4) return (i >> 1) & 0xFF;    // two pixels per halfword write
    if (x < 160 && y < 128) return (y * 160 + x) & 0xFFFF;
    return 0;                                 // mode 5 backdrop
}

std::vector<uint32_t> readFramebuffer(VkContext& ctx, InstancePool& pool, uint32_t instance) {
    std::vector<uint32_t> fb(FB_WORDS);
    downloadBuffer(ctx, pool.fb, fb.data(), FB_WORDS * 4,
                   VkDeviceSize(instance) * FB_WORDS * 4);
    return fb;
}

size_t countDifferences(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
    size_t n = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (a[i] != b[i]) ++n;
    return n;
}


// Runs one bitmap-mode ROM on both targets and checks the result.
void runMode(uint32_t mode, const std::vector<uint32_t>& rom) {
    std::printf("\n--- mode %u ---\n", mode);

    // ---- CPU reference -----------------------------------------------------
    host::MemoryPool hostPool;
    hostPool.allocate(1, uint32_t(rom.size()), /*withFramebuffers=*/true);
    hostPool.bind();
    host::installBios(hostPool.bios);
    hostPool.io[REG_KEYINPUT >> 2] = 0x03FF;  // KEYINPUT is active low: no keys held
    std::copy(rom.begin(), rom.end(), hostPool.rom.begin());
    g_flags = FLAG_RENDER;

    GbaState cpuState{};
    host::hleBoot(cpuState);

    // Run to the ROM's final self-branch, then one more full frame so every
    // scanline is drawn against the finished VRAM contents.
    uint32_t fillCycles = 0;
    for (; fillCycles < 5u * 1000 * 1000; ++fillCycles) {
        const uint32_t pc = cpuState.r[15];
        cpu_step(cpuState);
        if (cpuState.r[15] == pc) break;
    }
    const uint32_t totalCycles = fillCycles + 1 + CYCLES_PER_FRAME;
    for (uint32_t i = fillCycles + 1; i < totalCycles; ++i) cpu_step(cpuState);
    std::printf("  %u cycles to draw, +1 frame to render\n", fillCycles);

    const std::vector<uint32_t> cpuFb(hostPool.fb.begin(), hostPool.fb.begin() + FB_WORDS);

    // A renderer that never ran leaves the framebuffer zeroed, and one that
    // fills with the backdrop leaves it uniform. Both would sail through a
    // plain CPU-vs-GPU comparison, so check the content is real first.
    uint32_t distinct = 0;
    {
        std::vector<uint8_t> seen(65536, 0);
        for (uint32_t i = 0; i < SCREEN_W * SCREEN_H; ++i) {
            const uint32_t c = (cpuFb[i >> 1] >> ((i & 1) * 16)) & 0xFFFF;
            if (!seen[c]) { seen[c] = 1; ++distinct; }
        }
    }
    std::printf("  %u distinct colours\n", distinct);
    check(distinct > 100, "image is not blank or uniform");

    bool exact = true;
    for (uint32_t y = 0; y < SCREEN_H && exact; ++y) {
        for (uint32_t x = 0; x < SCREEN_W; ++x) {
            const uint32_t i = y * SCREEN_W + x;
            const uint32_t got = (cpuFb[i >> 1] >> ((i & 1) * 16)) & 0xFFFF;
            const uint32_t want = expectedPixel(mode, x, y);
            if (got != want) {
                std::printf("  pixel (%u,%u): got %04X want %04X\n", x, y, got, want);
                exact = false;
                break;
            }
        }
    }
    check(exact, "every pixel matches what the ROM drew");

    // ---- GPU ---------------------------------------------------------------
    VkContext ctx;
    ctx.init(/*validation=*/true, /*debugPrintf=*/false);
    InstancePool pool;
    pool.create(ctx, kInstances, uint32_t(rom.size()), /*withFramebuffers=*/true);
    uploadBuffer(ctx, pool.rom, rom.data(), rom.size() * 4);
    // The GPU needs the same BIOS: the interrupt path runs real ARM code from
    // the vector table, so a zeroed BIOS region would diverge from the CPU.
    uploadBuffer(ctx, pool.bios, hostPool.bios.data(), hostPool.bios.size() * 4);

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

    const std::vector<uint32_t> gpuFb = readFramebuffer(ctx, pool, 0);
    const size_t diff = countDifferences(cpuFb, gpuFb);
    if (diff) std::printf("  %zu/%u framebuffer words differ\n", diff, FB_WORDS);
    check(diff == 0, "GPU framebuffer matches the CPU reference");

    size_t divergentInstances = 0;
    for (uint32_t i = 1; i < kInstances; ++i)
        if (countDifferences(gpuFb, readFramebuffer(ctx, pool, i)) != 0) ++divergentInstances;
    check(divergentInstances == 0, "all instances rendered identically");

    const std::string path = std::string(OUTPUT_DIR) + "/mode" + std::to_string(mode) + ".png";
    check(host::writePng(path, host::bgr555ToRgb(gpuFb, SCREEN_W, SCREEN_H), SCREEN_W, SCREEN_H),
          "wrote the GPU framebuffer as a PNG");

    pipe.destroy(ctx);
    pool.destroy(ctx);
    ctx.destroy();
}

}  // namespace

int main() {
    runMode(3, buildMode3Rom());
    runMode(4, buildMode4Rom());
    runMode(5, buildMode5Rom());

    if (failures) {
        std::printf("\nM5 FAILED: %d checks failed\n", failures);
        return 1;
    }
    std::printf("\nM5 PASS: the GPU renders all three bitmap modes\n");
    std::printf("          see mode3.png, mode4.png, mode5.png\n");
    return 0;
}
