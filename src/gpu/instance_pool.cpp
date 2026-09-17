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

namespace {

// One instance's slice of a region. Contiguous in the default layout; the
// interleaved (GBA_SOA) build would need a strided gather here instead.
void readSlice(VkContext& ctx, Buffer& buf, uint32_t wordsPerInstance, uint32_t inst,
               std::vector<uint32_t>& out) {
    out.resize(wordsPerInstance);
    downloadBuffer(ctx, buf, out.data(), VkDeviceSize(wordsPerInstance) * 4,
                   VkDeviceSize(inst) * wordsPerInstance * 4);
}

void writeSlice(VkContext& ctx, Buffer& buf, uint32_t wordsPerInstance, uint32_t inst,
                const std::vector<uint32_t>& in) {
    uploadBuffer(ctx, buf, in.data(), VkDeviceSize(wordsPerInstance) * 4,
                 VkDeviceSize(inst) * wordsPerInstance * 4);
}

}  // namespace

void InstancePool::snapshotInstance(VkContext& ctx, uint32_t instance, Snapshot& out) {
    std::vector<GbaState> states;
    downloadStates(ctx, states);
    out.state = states[instance];

    readSlice(ctx, ewram, EWRAM_WORDS, instance, out.ewram);
    readSlice(ctx, iwram, IWRAM_WORDS, instance, out.iwram);
    readSlice(ctx, vram, VRAM_WORDS, instance, out.vram);
    readSlice(ctx, pram, PRAM_WORDS, instance, out.pram);
    readSlice(ctx, oam, OAM_WORDS, instance, out.oam);
    readSlice(ctx, io, IO_WORDS, instance, out.io);
    if (hasSave) readSlice(ctx, sram, SRAM_WORDS, instance, out.sram);
}

void InstancePool::restoreInstances(VkContext& ctx, const std::vector<uint32_t>& instances,
                                    const Snapshot& snap) {
    if (instances.empty()) return;

    // States are one array, so read it once, patch the entries and write once
    // rather than doing a round trip per instance.
    std::vector<GbaState> states;
    downloadStates(ctx, states);
    for (uint32_t i : instances) {
        if (i >= numInstances) continue;
        states[i] = snap.state;
        states[i].inst = i;  // the snapshot came from a different instance
    }
    uploadStates(ctx, states);

    for (uint32_t i : instances) {
        if (i >= numInstances) continue;
        writeSlice(ctx, ewram, EWRAM_WORDS, i, snap.ewram);
        writeSlice(ctx, iwram, IWRAM_WORDS, i, snap.iwram);
        writeSlice(ctx, vram, VRAM_WORDS, i, snap.vram);
        writeSlice(ctx, pram, PRAM_WORDS, i, snap.pram);
        writeSlice(ctx, oam, OAM_WORDS, i, snap.oam);
        writeSlice(ctx, io, IO_WORDS, i, snap.io);
        if (hasSave && !snap.sram.empty()) writeSlice(ctx, sram, SRAM_WORDS, i, snap.sram);
    }
}

void InstancePool::readProbes(VkContext& ctx, const std::vector<uint32_t>& addresses,
                              std::vector<uint32_t>& out) {
    out.assign(addresses.size() * numInstances, 0u);

    struct Target { Buffer* region; uint32_t words; uint32_t word; };
    std::vector<Target> targets(addresses.size(), Target{nullptr, 0, 0});
    for (size_t a = 0; a < addresses.size(); ++a) {
        const uint32_t addr = addresses[a];
        Target& t = targets[a];
        switch ((addr >> 24) & 0xF) {
            case 0x2: t = {&ewram, EWRAM_WORDS, (addr & (EWRAM_SIZE - 1)) >> 2}; break;
            case 0x3: t = {&iwram, IWRAM_WORDS, (addr & (IWRAM_SIZE - 1)) >> 2}; break;
            case 0x5: t = {&pram, PRAM_WORDS, (addr & (PRAM_SIZE - 1)) >> 2}; break;
            // VRAM mirrors on a 128 KiB window, not a power of two; see memmap.h.
            case 0x6: t = {&vram, VRAM_WORDS, vram_offset(addr) >> 2}; break;
            case 0x7: t = {&oam, OAM_WORDS, (addr & (OAM_SIZE - 1)) >> 2}; break;
            case 0x4: t = {&io, IO_WORDS, (addr & (IO_SIZE - 1)) >> 2}; break;
            default: break;  // ROM and BIOS are shared and constant; nothing to probe
        }
    }

    // Unified memory: gather straight out of the mapping, which costs nothing.
    if (ctx.unifiedMemory) {
        std::vector<uint32_t> one;
        for (size_t a = 0; a < addresses.size(); ++a) {
            if (!targets[a].region || !targets[a].region->mapped) continue;
            readProbe(ctx, *targets[a].region, targets[a].words, targets[a].word, one);
            std::copy(one.begin(), one.end(), out.begin() + long(a) * numInstances);
        }
        if (std::all_of(targets.begin(), targets.end(),
                        [](const Target& t) { return !t.region || t.region->mapped; }))
            return;
    }

    // A discrete GPU: every read through the mapping is a trip over PCIe, and a
    // reward function probing a few hundred bytes of RAM in every instance made
    // several hundred thousand of them a step. Instead, for each region, merge
    // the probed words into runs of adjacent words and copy every instance's
    // runs into staging with one submission.
    for (Buffer* region : {&ewram, &iwram, &pram, &vram, &oam, &io}) {
        std::vector<uint32_t> words;
        uint32_t perInstance = 0;
        for (const Target& t : targets)
            if (t.region == region) { words.push_back(t.word); perInstance = t.words; }
        if (words.empty() || (ctx.unifiedMemory && region->mapped)) continue;
        std::sort(words.begin(), words.end());
        words.erase(std::unique(words.begin(), words.end()), words.end());

        struct Run { uint32_t first, count, staged; };  // staged: offset within one instance's block
        std::vector<Run> runs;
        uint32_t blockWords = 0;
        for (uint32_t w : words) {
            if (!runs.empty() && runs.back().first + runs.back().count == w) {
                ++runs.back().count;
            } else {
                runs.push_back({w, 1, blockWords});
            }
            ++blockWords;
        }

        std::vector<VkBufferCopy> copies;
        copies.reserve(size_t(runs.size()) * numInstances);
        for (uint32_t i = 0; i < numInstances; ++i)
            for (const Run& r : runs)
                copies.push_back({(VkDeviceSize(i) * perInstance + r.first) * 4,
                                  (VkDeviceSize(i) * blockWords + r.staged) * 4, VkDeviceSize(r.count) * 4});
        std::vector<uint32_t> staged(size_t(blockWords) * numInstances);
        downloadRegions(ctx, *region, copies, VkDeviceSize(staged.size()) * 4, staged.data());

        for (size_t a = 0; a < addresses.size(); ++a) {
            if (targets[a].region != region) continue;
            const uint32_t pos = uint32_t(std::lower_bound(words.begin(), words.end(), targets[a].word) - words.begin());
            for (uint32_t i = 0; i < numInstances; ++i)
                out[a * numInstances + i] = staged[size_t(i) * blockWords + pos];
        }
    }
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
