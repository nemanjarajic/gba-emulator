#include "api/gba_env.h"

#include "core/core.inc"
#include "gpu/instance_pool.h"
#include "gpu/vk_context.h"
#include "host/memory.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace gba;

namespace {

std::string g_error;
constexpr uint32_t kLocalSize = 64;  // must match gba.comp

void setError(std::string what) { g_error = std::move(what); }

}  // namespace

struct GbaEnv {
    VkContext ctx;
    InstancePool pool;
    ComputePipeline pipe;
    std::vector<uint32_t> rom;
    uint32_t instances = 0;
    uint32_t flags = 0;

    InstancePool::Snapshot resetPoint;
    bool haveResetPoint = false;

    std::vector<uint32_t> keyinput;   // staged actions, active low
    std::vector<uint8_t> observations;
    std::vector<uint32_t> framebuffer;

    uint32_t groups() const { return (instances + kLocalSize - 1) / kLocalSize; }

    // Cycles per dispatch. Windows resets the GPU when one command runs for
    // about two seconds (TDR): on an RTX 5060 Ti a 1.5 s dispatch survives and
    // a 2 s one loses the device. A whole frame of Emerald at 9280 instances
    // already takes 1.7 s, so frames are split, and the split adapts to
    // measured time: it starts small, doubles while dispatches are quick and
    // halves when one is slow. GBA_ENV_DISPATCH_CYCLES fixes it instead.
    uint32_t chunkCycles = CYCLES_PER_FRAME / 16;
    bool chunkFixed = false;

    void dispatch(uint32_t cycles, uint32_t extraFlags) {
        constexpr double kTargetSeconds = 0.25;
        constexpr uint32_t kMinChunk = 1024;
        // Rendering happens scanline by scanline as the machine runs, so a
        // frame split across dispatches draws exactly the same picture. The
        // observation is sampled at the end of a dispatch, so only the last
        // one takes it.
        const uint32_t observe = extraFlags & FLAG_OBSERVE;
        const uint32_t flags = pool.baseFlags() | (extraFlags & ~FLAG_OBSERVE);
        for (uint32_t done = 0; done < cycles;) {
            const uint32_t chunk = std::min(chunkCycles, cycles - done);
            done += chunk;
            CorePush push{instances, uint32_t(rom.size()), chunk,
                          flags | (done == cycles ? observe : 0u)};
            const auto t0 = std::chrono::steady_clock::now();
            dispatchBlocking(ctx, pipe, groups(), &push, sizeof(push));
            if (chunkFixed) continue;
            const double secs =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (secs > kTargetSeconds && chunkCycles / 2 >= kMinChunk)
                chunkCycles /= 2;
            else if (secs < kTargetSeconds / 4 && chunk == chunkCycles &&
                     chunkCycles < CYCLES_PER_FRAME)
                chunkCycles *= 2;
        }
    }
};

extern "C" {

uint32_t gba_env_abi_version(void) { return GBA_ENV_ABI_VERSION; }

const char* gba_env_last_error(void) { return g_error.c_str(); }
uint32_t gba_env_obs_width(void) { return OBS_W; }
uint32_t gba_env_obs_height(void) { return OBS_H; }
uint32_t gba_env_num_instances(const GbaEnv* env) { return env ? env->instances : 0; }

GbaEnv* gba_env_create(const char* rom_path, uint32_t num_instances, uint32_t flags) {
    if (!rom_path || num_instances == 0) {
        setError("gba_env_create: need a rom path and at least one instance");
        return nullptr;
    }
    auto* env = new GbaEnv();
    env->instances = num_instances;
    env->flags = flags;
    if (const char* fixed = std::getenv("GBA_ENV_DISPATCH_CYCLES")) {
        env->chunkCycles = std::max(uint32_t(std::strtoul(fixed, nullptr, 10)), 1u);
        env->chunkFixed = true;
    }

    if (!host::loadBinary(rom_path, env->rom)) {
        setError(std::string("cannot read ") + rom_path);
        delete env;
        return nullptr;
    }

    env->ctx.init(/*validation=*/false, /*debugPrintf=*/false);

    const bool withSave = (flags & GBA_ENV_NO_SAVE) == 0;
    if (!InstancePool::fits(env->ctx, num_instances, uint32_t(env->rom.size()),
                            /*withFramebuffers=*/true, withSave, /*verbose=*/false)) {
        setError("the requested instance count does not fit on this device; "
                 "see stderr for the limit that was exceeded");
        env->ctx.destroy();
        delete env;
        return nullptr;
    }

    env->pool.create(env->ctx, num_instances, uint32_t(env->rom.size()),
                     /*withFramebuffers=*/true, withSave);
    uploadBuffer(env->ctx, env->pool.rom, env->rom.data(), env->rom.size() * 4);

    std::vector<uint32_t> bios;
    host::installBios(bios);
    uploadBuffer(env->ctx, env->pool.bios, bios.data(), bios.size() * 4);

    env->pipe.create(env->ctx, std::string(SHADER_DIR) + "/gba.spv",
                     uint32_t(env->pool.bindings().size()), sizeof(CorePush));
    env->pipe.bindBuffers(env->ctx, env->pool.bindings());

    env->keyinput.assign(num_instances, 0x03FFu);
    env->observations.assign(size_t(num_instances) * OBS_W * OBS_H, 0u);
    env->framebuffer.assign(FB_WORDS, 0u);

    gba_env_reset_all(env);
    return env;
}

void gba_env_destroy(GbaEnv* env) {
    if (!env) return;
    env->pipe.destroy(env->ctx);
    env->pool.destroy(env->ctx);
    env->ctx.destroy();
    delete env;
}

int gba_env_capture_reset_point(GbaEnv* env, uint32_t instance) {
    if (!env || instance >= env->instances) {
        setError("capture_reset_point: instance out of range");
        return 0;
    }
    env->pool.snapshotInstance(env->ctx, instance, env->resetPoint);
    env->haveResetPoint = true;
    return 1;
}

void gba_env_reset(GbaEnv* env, const uint32_t* instances, uint32_t count) {
    if (!env || !instances || count == 0) return;
    const std::vector<uint32_t> which(instances, instances + count);

    if (env->haveResetPoint) {
        env->pool.restoreInstances(env->ctx, which, env->resetPoint);
        return;
    }
    // No reset point captured: put the listed instances back to a cold boot.
    // Memory is not cleared here, because a cold boot does not clear it either
    // -- the BIOS does, and the game will.
    std::vector<GbaState> states;
    env->pool.downloadStates(env->ctx, states);
    for (uint32_t i : which) {
        if (i >= env->instances) continue;
        states[i] = GbaState{};
        host::hleBoot(states[i]);
        states[i].inst = i;
    }
    env->pool.uploadStates(env->ctx, states);
}

void gba_env_reset_all(GbaEnv* env) {
    if (!env) return;
    std::vector<uint32_t> all(env->instances);
    for (uint32_t i = 0; i < env->instances; ++i) all[i] = i;
    gba_env_reset(env, all.data(), uint32_t(all.size()));
}

void gba_env_step(GbaEnv* env, const uint16_t* actions, uint32_t frames) {
    if (!env || frames == 0) return;

    if (actions) {
        // An action names the buttons held; the register is active low.
        for (uint32_t i = 0; i < env->instances; ++i)
            env->keyinput[i] = 0x03FFu & ~uint32_t(actions[i] & 0x03FFu);
    }
    env->pool.setInputs(env->ctx, env->keyinput);

    // Only the last frame is rendered and observed. Rendering has no effect on
    // the emulated machine, so skipping it on repeated frames is free accuracy
    // rather than a compromise.
    for (uint32_t f = 0; f + 1 < frames; ++f) env->dispatch(CYCLES_PER_FRAME, 0);
    env->dispatch(CYCLES_PER_FRAME, FLAG_RENDER | FLAG_OBSERVE);

    env->pool.readObservations(env->ctx, env->observations);
}

const uint8_t* gba_env_observations(GbaEnv* env) {
    return env ? env->observations.data() : nullptr;
}

void gba_env_read_probes(GbaEnv* env, const uint32_t* addresses, uint32_t count, uint32_t* out) {
    if (!env || !addresses || !out || count == 0) return;
    std::vector<uint32_t> result;
    env->pool.readProbes(env->ctx, std::vector<uint32_t>(addresses, addresses + count), result);
    std::memcpy(out, result.data(), result.size() * sizeof(uint32_t));
}

const uint32_t* gba_env_framebuffer(GbaEnv* env, uint32_t instance) {
    if (!env || instance >= env->instances) return nullptr;
    downloadBuffer(env->ctx, env->pool.fb, env->framebuffer.data(), FB_WORDS * 4,
                   VkDeviceSize(instance) * FB_WORDS * 4);
    return env->framebuffer.data();
}

int gba_env_save_reset_point(GbaEnv* env, const char* path) {
    if (!env || !env->haveResetPoint) {
        setError("save_reset_point: nothing captured yet");
        return 0;
    }
    std::ofstream f(path, std::ios::binary);
    if (!f) {
        setError(std::string("cannot write ") + path);
        return 0;
    }
    const auto& s = env->resetPoint;
    auto put = [&](const std::vector<uint32_t>& v) {
        const uint32_t n = uint32_t(v.size());
        f.write(reinterpret_cast<const char*>(&n), 4);
        f.write(reinterpret_cast<const char*>(v.data()), std::streamsize(n) * 4);
    };
    f.write(reinterpret_cast<const char*>(&s.state), sizeof(GbaState));
    put(s.ewram); put(s.iwram); put(s.vram); put(s.pram); put(s.oam); put(s.io); put(s.sram);
    return f.good() ? 1 : 0;
}

int gba_env_load_reset_point(GbaEnv* env, const char* path) {
    if (!env) return 0;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        setError(std::string("cannot read ") + path);
        return 0;
    }
    auto& s = env->resetPoint;
    auto get = [&](std::vector<uint32_t>& v) {
        uint32_t n = 0;
        f.read(reinterpret_cast<char*>(&n), 4);
        v.resize(n);
        f.read(reinterpret_cast<char*>(v.data()), std::streamsize(n) * 4);
    };
    f.read(reinterpret_cast<char*>(&s.state), sizeof(GbaState));
    get(s.ewram); get(s.iwram); get(s.vram); get(s.pram); get(s.oam); get(s.io); get(s.sram);
    if (!f.good()) {
        setError(std::string("truncated reset point in ") + path);
        return 0;
    }
    env->haveResetPoint = true;
    return 1;
}

}  // extern "C"
