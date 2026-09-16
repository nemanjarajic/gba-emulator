#include "gpu/instance_pool.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace gba {

uint64_t InstancePool::bytesPerInstance(bool withFramebuffers, bool withSave) {
    uint64_t words = EWRAM_WORDS + IWRAM_WORDS + VRAM_WORDS + PRAM_WORDS + OAM_WORDS + IO_WORDS;
    if (withSave) words += SRAM_WORDS;
    if (withFramebuffers) words += FB_WORDS;
    words += OBS_WORDS;
    return words * sizeof(uint32_t) + sizeof(GbaState) + sizeof(uint32_t);
}

bool InstancePool::fits(const VkContext& ctx, uint32_t instances, uint32_t romWords,
                        bool withFramebuffers, bool withSave, bool verbose) {
    const auto& lim = ctx.props.limits;
    bool ok = true;

    // The pool binds one storage buffer per memory region.
    const uint32_t buffers = 13;
    if (buffers > lim.maxPerStageDescriptorStorageBuffers) {
        std::fprintf(stderr, "[pool] needs %u storage buffers, device allows %u\n", buffers,
                     lim.maxPerStageDescriptorStorageBuffers);
        ok = false;
    }

    // EWRAM is the largest region, so it is the one that hits a per-buffer cap
    // first: 256 KiB per instance means 4 GiB at 16384 instances, which is
    // exactly where a typical maxStorageBufferRange sits.
    const uint64_t largest = uint64_t(EWRAM_WORDS) * sizeof(uint32_t) * instances;
    if (largest > lim.maxStorageBufferRange) {
        std::fprintf(stderr,
                     "[pool] EWRAM would be %.2f GiB, above maxStorageBufferRange of %.2f GiB; "
                     "use at most %u instances\n",
                     double(largest) / (1u << 30), double(lim.maxStorageBufferRange) / (1u << 30),
                     uint32_t(lim.maxStorageBufferRange / (EWRAM_WORDS * sizeof(uint32_t))));
        ok = false;
    }

    const uint64_t total = bytesPerInstance(withFramebuffers, withSave) * instances +
                           uint64_t(std::max(romWords, 1u)) * sizeof(uint32_t) +
                           uint64_t(BIOS_WORDS) * sizeof(uint32_t);

    VkDeviceSize heap = 0;
    for (uint32_t i = 0; i < ctx.memProps.memoryHeapCount; ++i)
        if (ctx.memProps.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            heap = std::max(heap, ctx.memProps.memoryHeaps[i].size);

    // Leave room for the driver, the display and fragmentation. Allocating to
    // the last byte of a card that is also driving a monitor does not end well.
    const double usable = double(heap) * 0.85;
    if (double(total) > usable) {
        std::fprintf(stderr,
                     "[pool] %u instances need %.2f GiB, device-local heap is %.2f GiB "
                     "(usable ~%.2f GiB); fits about %u instances\n",
                     instances, double(total) / (1u << 30), double(heap) / (1u << 30),
                     usable / (1u << 30),
                     uint32_t(usable / double(bytesPerInstance(withFramebuffers, withSave))));
        ok = false;
    }

    if (verbose && ok)
        std::printf("[pool] %u instances, %.2f GiB of %.2f GiB heap\n", instances,
                    double(total) / (1u << 30), double(heap) / (1u << 30));
    return ok;
}

void InstancePool::create(VkContext& ctx, uint32_t instances, uint32_t romWords,
                          bool withFramebuffers, bool withSave) {
    if (!fits(ctx, instances, romWords, withFramebuffers, withSave, /*verbose=*/false)) {
        std::fprintf(stderr, "[pool] refusing to allocate; see the limits reported above\n");
        std::abort();
    }
    numInstances = instances;
    hasSave = withSave;
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
    sram = createStorageBuffer(ctx, withSave ? words(SRAM_WORDS)
                                             : VkDeviceSize(instances) * sizeof(uint32_t));
    fb = createStorageBuffer(ctx, withFramebuffers ? words(FB_WORDS)
                                                   : VkDeviceSize(instances) * sizeof(uint32_t));
    state = createStorageBuffer(ctx, VkDeviceSize(instances) * sizeof(GbaState));
    input = createStorageBuffer(ctx, VkDeviceSize(instances) * sizeof(uint32_t));
    obs = createStorageBuffer(ctx, words(OBS_WORDS));

    // Zeroed on the GPU rather than through a mapped pointer, so this works
    // unchanged when the buffers are device-local and unmappable.
    for (Buffer* b : bindings()) fillBuffer(ctx, *b, 0u);
    // Save memory powers on erased, not zeroed.
    if (withSave) fillBuffer(ctx, sram, 0xFFFFFFFFu);

    // Nothing held, until the harness says otherwise.
    {
        std::vector<uint32_t> released(instances, 0x03FFu);
        uploadBuffer(ctx, input, released.data(), released.size() * sizeof(uint32_t));
    }

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
    return {&bios, &rom, &ewram,  &iwram, &vram,  &pram, &oam,
            &io,   &sram, &fb,     &state, &input, &obs};
}

void InstancePool::uploadStates(VkContext& ctx, const std::vector<GbaState>& states) {
    uploadBuffer(ctx, state, states.data(), states.size() * sizeof(GbaState));
}

void InstancePool::downloadStates(VkContext& ctx, std::vector<GbaState>& states) {
    states.resize(numInstances);
    downloadBuffer(ctx, state, states.data(), states.size() * sizeof(GbaState));
}

void InstancePool::setInputs(VkContext& ctx, const std::vector<uint32_t>& keyinput) {
    uploadBuffer(ctx, input, keyinput.data(),
                 std::min<size_t>(keyinput.size(), numInstances) * sizeof(uint32_t));
}

void InstancePool::readObservations(VkContext& ctx, std::vector<uint8_t>& out) {
    out.resize(size_t(numInstances) * OBS_W * OBS_H);
    downloadBuffer(ctx, obs, out.data(), out.size());
}

void InstancePool::readProbe(VkContext& ctx, Buffer& region, uint32_t wordsPerInstance,
                             uint32_t wordIndex, std::vector<uint32_t>& out) {
    out.resize(numInstances);
    if (region.mapped) {
        // Unified memory or Resizable BAR: gather straight out of the mapping.
        const auto* words = static_cast<const uint32_t*>(region.mapped);
        for (uint32_t i = 0; i < numInstances; ++i)
            out[i] = words[size_t(i) * wordsPerInstance + wordIndex];
        return;
    }
    // Device-local only: one small transfer per instance. Slow, but this path
    // exists for correctness on a discrete GPU rather than for throughput.
    for (uint32_t i = 0; i < numInstances; ++i)
        downloadBuffer(ctx, region, &out[i], sizeof(uint32_t),
                       (VkDeviceSize(i) * wordsPerInstance + wordIndex) * sizeof(uint32_t));
}

uint64_t InstancePool::totalBytes() const {
    return bios.size + rom.size + ewram.size + iwram.size + vram.size + pram.size + oam.size +
           io.size + sram.size + fb.size + state.size + input.size + obs.size;
}

void InstancePool::destroy(VkContext& ctx) {
    for (Buffer* b : bindings()) destroyBuffer(ctx, *b);
    numInstances = 0;
}

}  // namespace gba
