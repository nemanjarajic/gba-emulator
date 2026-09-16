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

    // Zero everything. Host-visible unified memory means this is a plain memset
    // with no staging buffer or transfer submission.
    for (Buffer* b : bindings()) std::fill_n(static_cast<uint8_t*>(b->mapped), b->size, uint8_t(0));
    // Save memory powers on erased, not zeroed.
    std::fill_n(static_cast<uint8_t*>(sram.mapped), sram.size, uint8_t(0xFF));
}

std::vector<Buffer*> InstancePool::bindings() {
    return {&bios, &rom, &ewram, &iwram, &vram, &pram, &oam, &io, &sram, &fb};
}

uint64_t InstancePool::totalBytes() const {
    return bios.size + rom.size + ewram.size + iwram.size + vram.size + pram.size + oam.size +
           io.size + sram.size + fb.size;
}

void InstancePool::destroy(VkContext& ctx) {
    for (Buffer* b : bindings()) destroyBuffer(ctx, *b);
    numInstances = 0;
}

}  // namespace gba
