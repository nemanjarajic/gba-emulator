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
    bool hasSave = true;
    Buffer bios, rom, ewram, iwram, vram, pram, oam, io, sram, fb, state, input, obs;

    // `withSave` allocates 128 KiB of flash per instance. That is a quarter of
    // the whole per-instance footprint, and a throughput workload that never
    // saves can reclaim it -- pass FLAG_NO_SAVE in the dispatch flags to match.
    void create(VkContext& ctx, uint32_t instances, uint32_t romWords, bool withFramebuffers,
                bool withSave = true);

    // Bytes one instance costs, for budgeting against a card's VRAM.
    static uint64_t bytesPerInstance(bool withFramebuffers, bool withSave);

    // Checks the pool against the device's limits and prints what is wrong.
    // Returns false instead of aborting, so a caller can retry with fewer
    // instances rather than dying.
    static bool fits(const VkContext& ctx, uint32_t instances, uint32_t romWords,
                     bool withFramebuffers, bool withSave, bool verbose);
    void destroy(VkContext& ctx);

    // Dispatch flags this pool requires. Passing FLAG_NO_SAVE is not optional
    // when the pool was built without save memory: the shader would otherwise
    // index a buffer that is 128 KiB per instance too small. Always start from
    // this rather than from zero.
    uint32_t baseFlags() const { return hasSave ? 0u : FLAG_NO_SAVE; }

    // Descriptor binding order; see state.h.
    std::vector<Buffer*> bindings();

    // Writes every instance's starting state, and reads it all back.
    void uploadStates(VkContext& ctx, const std::vector<GbaState>& states);
    void downloadStates(VkContext& ctx, std::vector<GbaState>& states);

    // --- throughput harness -------------------------------------------------

    // One KEYINPUT value per instance, active low (0x03FF = nothing held).
    // Applied at the start of the next dispatch.
    void setInputs(VkContext& ctx, const std::vector<uint32_t>& keyinput);

    // Reads every instance's downsampled observation. The result is
    // numInstances * OBS_W * OBS_H bytes, one instance after another.
    void readObservations(VkContext& ctx, std::vector<uint8_t>& out);

    // --- snapshots and per-instance reset -----------------------------------

    // One instance's entire machine: registers and every memory region. This is
    // what an episode reset restores, and it has to include memory -- restoring
    // registers alone leaves the game's own state behind and the next episode
    // starts mid-scene.
    struct Snapshot {
        GbaState state{};
        std::vector<uint32_t> ewram, iwram, vram, pram, oam, io, sram;
    };

    void snapshotInstance(VkContext& ctx, uint32_t instance, Snapshot& out);

    // Restores a snapshot into the listed instances, leaving every other
    // instance untouched. Episodes end at different times, so resetting the
    // whole pool is not an option.
    void restoreInstances(VkContext& ctx, const std::vector<uint32_t>& instances,
                          const Snapshot& snap);

    // Gathers one word from the same offset in every instance's slice of a
    // region -- the cheap way to poll a reward signal or a game-state variable
    // without reading back whole regions.
    void readProbe(VkContext& ctx, Buffer& region, uint32_t wordsPerInstance,
                   uint32_t wordIndex, std::vector<uint32_t>& out);

    // The same, addressed by GBA address rather than by region and index, and
    // for several addresses at once. A reward function usually reads more than
    // one variable. `out` is filled with one word per address per instance,
    // address-major: out[a * numInstances + i].
    void readProbes(VkContext& ctx, const std::vector<uint32_t>& addresses,
                    std::vector<uint32_t>& out);

    // Total bytes allocated, for reporting the instance-count budget.
    uint64_t totalBytes() const;
};

}  // namespace gba
