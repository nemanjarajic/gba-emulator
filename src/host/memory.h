#pragma once

// Host-side backing store for the core's g_* storage regions.
//
// On the GPU these regions are Vulkan storage buffers; here they are plain
// vectors, and binding one simply points the globals at them. The core code
// cannot tell the difference, which is the whole point.

#include "core/core.h"

#include <cstdint>
#include <string>
#include <vector>

namespace gba::host {

struct MemoryPool {
    uint32_t numInstances = 1;
    std::vector<uint32_t> bios, rom, ewram, iwram, vram, pram, oam, io, sram, fb, input, obs;

    // `withFramebuffers` is opt-in: at high instance counts a framebuffer per
    // instance costs more than the rest of the machine state put together and
    // most throughput workloads never look at the pixels.
    void allocate(uint32_t instances, uint32_t romWords, bool withFramebuffers);

    // Points the core's globals at this pool. Exactly one pool is bound at a
    // time, mirroring the single descriptor set the GPU path uses.
    void bind();
};

// Loads a file into a word vector, zero-padding to a word boundary.
// Returns false if the file cannot be read.
bool loadBinary(const std::string& path, std::vector<uint32_t>& out);

// Fills a BIOS image with a minimal but real vector table.
//
// Most BIOS calls are handled high-level in bios.inc, but the interrupt path
// cannot be: a game reaches its own handler because the BIOS loads a pointer
// from 0x03007FFC and jumps through it. That has to be executed as ARM code.
void installBios(std::vector<uint32_t>& bios);

// Sets up registers the way the BIOS would have left them, so a ROM can be
// booted without a BIOS image. Enough for the CPU test ROMs used in M2/M3.
void hleBoot(GbaState& st);

}  // namespace gba::host
