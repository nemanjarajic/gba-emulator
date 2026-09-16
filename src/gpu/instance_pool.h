#pragma once

// GPU-side backing store for the core's storage regions: the Vulkan twin of
// host::MemoryPool. Binding order here MUST match the layout(binding = N)
// declarations in src/core/state.h.

#include "core/core.h"
#include "gpu/vk_context.h"

#include <cstdint>
#include <vector>

namespace gba {

// Mirrors the push_constant block in src/core/state.h.
struct CorePush {
    uint32_t numInstances;
    uint32_t romWords;
    uint32_t cycles;
    uint32_t flags;
};

struct InstancePool {
    uint32_t numInstances = 0;
    Buffer bios, rom, ewram, iwram, vram, pram, oam, io, sram, fb, state;

    void create(VkContext& ctx, uint32_t instances, uint32_t romWords, bool withFramebuffers);
    void destroy(VkContext& ctx);

    // Descriptor binding order; see state.h.
    std::vector<Buffer*> bindings();

    // Writes every instance's starting state, and reads it all back.
    void uploadStates(VkContext& ctx, const std::vector<GbaState>& states);
    void downloadStates(VkContext& ctx, std::vector<GbaState>& states);

    // Total bytes allocated, for reporting the instance-count budget.
    uint64_t totalBytes() const;
};

}  // namespace gba
