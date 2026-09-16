// Exercises the C ABI in src/api/gba_env.h.
//
// The bindings that consume this interface live in a separate repository, so
// this repository has to test the interface itself. Without it, a change to
// gba_env.h would only be caught by whoever loaded the library next.
//
// Uses build/roms/input_echo.gba, a ROM that publishes its controller state to
// EWRAM word 0 every loop, which makes the action path verifiable from outside
// the emulator: whatever action goes in must come back out of the emulated
// machine's own memory.

#include "api/gba_env.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-56s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string rom = argc > 1 ? argv[1] : "build/roms/input_echo.gba";
    constexpr uint32_t kN = 64;

    check(gba_env_abi_version() == GBA_ENV_ABI_VERSION,
          "the library reports the ABI version this was built against");

    GbaEnv* env = gba_env_create(rom.c_str(), kN, 0);
    if (!env) {
        std::printf("gba_env_create failed: %s\n", gba_env_last_error());
        std::printf("(run: python3 tools/make_bench_roms.py)\n");
        return 2;
    }

    const uint32_t w = gba_env_obs_width(), h = gba_env_obs_height();
    std::printf("%s: %u instances, observations %ux%u\n\n", rom.c_str(),
                gba_env_num_instances(env), w, h);
    check(gba_env_num_instances(env) == kN, "the instance count is what was asked for");

    gba_env_reset_all(env);
    const std::vector<uint8_t> atReset(gba_env_observations(env),
                                       gba_env_observations(env) + size_t(kN) * w * h);
    uint8_t maxAtReset = 0;
    for (uint8_t v : atReset) maxAtReset = std::max(maxAtReset, v);
    check(maxAtReset == 0, "nothing has been drawn before the first step");

    // Every instance gets a different action.
    std::vector<uint16_t> actions(kN);
    for (uint32_t i = 0; i < kN; ++i) actions[i] = uint16_t((i * 37u + 11u) & 0x03FFu);
    gba_env_step(env, actions.data(), 4);

    const uint32_t probeAddr = 0x02000000;
    std::vector<uint32_t> echoed(kN);
    gba_env_read_probes(env, &probeAddr, 1, echoed.data());

    uint32_t wrong = 0;
    for (uint32_t i = 0; i < kN; ++i)
        if (echoed[i] != (0x03FFu & ~uint32_t(actions[i]))) ++wrong;
    check(wrong == 0, "every instance read back the action it was given");

    const uint8_t* obs = gba_env_observations(env);
    uint8_t brightest = 0;
    for (uint32_t i = 0; i < w * h; ++i) brightest = std::max(brightest, obs[i]);
    check(brightest == 255, "the observation contains the ROM's white band");

    // A reset point is a whole machine, memory included.
    check(gba_env_capture_reset_point(env, 0) != 0, "a reset point can be captured");
    const uint32_t before = echoed[0];

    std::vector<uint16_t> zero(kN, 0);
    gba_env_step(env, zero.data(), 4);
    gba_env_read_probes(env, &probeAddr, 1, echoed.data());
    check(echoed[0] != before, "stepping changes the machine");
    const uint32_t moved = echoed[0];

    const uint32_t only0 = 0;
    gba_env_reset(env, &only0, 1);
    gba_env_read_probes(env, &probeAddr, 1, echoed.data());
    check(echoed[0] == before, "reset restores the captured machine");

    uint32_t disturbed = 0;
    for (uint32_t i = 1; i < kN; ++i)
        if (echoed[i] != moved) ++disturbed;
    check(disturbed == 0, "resetting one instance leaves the others alone");

    const std::string statePath = "build/api_test.state";
    check(gba_env_save_reset_point(env, statePath.c_str()) != 0, "a reset point can be saved");
    gba_env_step(env, zero.data(), 4);
    check(gba_env_load_reset_point(env, statePath.c_str()) != 0, "and loaded back");
    gba_env_reset(env, &only0, 1);
    gba_env_read_probes(env, &probeAddr, 1, echoed.data());
    check(echoed[0] == before, "a reset point survives a save and load");
    std::remove(statePath.c_str());

    check(gba_env_framebuffer(env, 0) != nullptr, "a framebuffer can be read back");
    check(gba_env_framebuffer(env, kN) == nullptr, "an out-of-range instance is refused");

    gba_env_destroy(env);

    std::printf("\n");
    if (failures) {
        std::printf("API TEST FAILED: %d checks failed\n", failures);
        return 1;
    }
    std::printf("API TEST PASS\n");
    return 0;
}
