#include "gpu/instance_pool.h"

#include <algorithm>

namespace gba {

void InstancePool::create(VkContext& ctx, uint32_t instances, uint32_t romWords,
                          bool withFramebuffers) {
    numInstances = instances;
    const auto words = [&](uint32_t perInstance) -> VkDeviceSize {
        return VkDeviceSize(perInstance) * instances * sizeof(uint32_t);
    };

    // BIOS and ROM are shared across every instance and read-only. This is what
    // makes high instance counts affordable: a 32 MiB cartridge is stored once,
    // not once per machine.
    bios = createStorageBuffer(ctx, VkDeviceSize(BIOS_WORDS) * sizeof(uint32_t));
    rom = createStorageBuffer(ctx, VkDeviceSize(std::max(romWords, 1u)) * sizeof(uint32_t));

    ewram = createStorageBuffer(ctx, words(EWRAM_WORDS));
    iwram = createStorageBuffer(ctx, words(IWRAM_WORDS));
    vram = createStorageBuffer(ctx, words(VRAM_WORDS));
    pram = createStorageBuffer(ctx, words(PRAM_WORDS));
    oam = createStorageBuffer(ctx, words(OAM_WORDS));
    io = createStorageBuffer(ctx, words(IO_WORDS));
    sram = createStorageBuffer(ctx, words(SRAM_WORDS));
    fb = createStorageBuffer(ctx, withFramebuffers ? words(FB_WORDS)
                                                   : VkDeviceSize(instances) * sizeof(uint32_t));
    state = createStorageBuffer(ctx, VkDeviceSize(instances) * sizeof(GbaState));

    // Zeroed on the GPU rather than through a mapped pointer, so this works
    // unchanged when the buffers are device-local and unmappable.
    for (Buffer* b : bindings()) fillBuffer(ctx, *b, 0u);
    // Save memory powers on erased, not zeroed.
    fillBuffer(ctx, sram, 0xFFFFFFFFu);

    // KEYINPUT is active low, so a zeroed I/O region means every button is
    // held. Games take very different boot paths when they see that -- the
    // soft-reset combination is buttons-held -- so this must be set before an
    // instance runs a single instruction.
    std::vector<uint32_t> ioImage(IO_WORDS, 0u);
    ioImage[REG_KEYINPUT >> 2] = 0x03FFu;
    for (uint32_t i = 0; i < instances; ++i)
        uploadBuffer(ctx, io, ioImage.data(), ioImage.size() * sizeof(uint32_t),
                     VkDeviceSize(i) * IO_WORDS * sizeof(uint32_t));
}

std::vector<Buffer*> InstancePool::bindings() {
    return {&bios, &rom, &ewram, &iwram, &vram, &pram, &oam, &io, &sram, &fb, &state};
}

void InstancePool::uploadStates(VkContext& ctx, const std::vector<GbaState>& states) {
    uploadBuffer(ctx, state, states.data(), states.size() * sizeof(GbaState));
}

void InstancePool::downloadStates(VkContext& ctx, std::vector<GbaState>& states) {
    states.resize(numInstances);
    downloadBuffer(ctx, state, states.data(), states.size() * sizeof(GbaState));
}

uint64_t InstancePool::totalBytes() const {
    return bios.size + rom.size + ewram.size + iwram.size + vram.size + pram.size + oam.size +
           io.size + sram.size + fb.size + state.size;
}

void InstancePool::destroy(VkContext& ctx) {
    for (Buffer* b : bindings()) destroyBuffer(ctx, *b);
    numInstances = 0;
}

}  // namespace gba
