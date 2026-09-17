// Builds a reset point on the CPU reference core by playing an input script.
//
//   make_state <rom> <script> <out.state> [--from <in.state>] [--png <out.png>]
//
// Getting a game to where episodes should start -- past a title screen and an
// intro -- is sequential work, which is the one thing the GPU is bad at: each
// GPU instance runs at a small fraction of real time. The CPU core runs a
// single machine at several times real time, so the start state is made here
// and loaded by the environment with gba_env_load_reset_point.
//
// The script is one command per line; '#' starts a comment.
//   <buttons> <frames> [x<repeat>]   hold buttons for frames, repeat the pair
//   mash <buttons> <count>           press and release, 4 frames each, count times
// Buttons are joined with '+' from: A B SELECT START RIGHT LEFT UP DOWN R L,
// or 'none'. For example:
//   none 600          # let the intro play
//   mash A 300        # through the dialogue
//   START 4           # open the menu
//
// The file format matches gba_env_save_reset_point: the GbaState, then EWRAM,
// IWRAM, VRAM, PRAM, OAM, I/O and save memory, each as a word count and words.

#include "core/core.inc"
#include "host/memory.h"
#include "host/png.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace gba;

namespace {

bool parseButtons(const std::string& text, uint32_t& mask) {
    static const char* kNames[] = {"A", "B", "SELECT", "START", "RIGHT", "LEFT", "UP", "DOWN", "R", "L"};
    mask = 0;
    if (text == "none") return true;
    std::stringstream ss(text);
    std::string name;
    while (std::getline(ss, name, '+')) {
        bool found = false;
        for (uint32_t i = 0; i < 10; ++i)
            if (name == kNames[i]) { mask |= 1u << i; found = true; }
        if (!found) return false;
    }
    return true;
}

void putWords(std::ofstream& f, const std::vector<uint32_t>& v) {
    const uint32_t n = uint32_t(v.size());
    f.write(reinterpret_cast<const char*>(&n), 4);
    f.write(reinterpret_cast<const char*>(v.data()), std::streamsize(n) * 4);
}

bool getWords(std::ifstream& f, std::vector<uint32_t>& v) {
    uint32_t n = 0;
    f.read(reinterpret_cast<char*>(&n), 4);
    if (!f || n != v.size()) return false;  // must match this pool's layout
    f.read(reinterpret_cast<char*>(v.data()), std::streamsize(n) * 4);
    return bool(f);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: make_state <rom> <script> <out.state> [--from <in.state>] [--png <out.png>]\n");
        return 2;
    }
    const std::string romPath = argv[1], scriptPath = argv[2], outPath = argv[3];
    std::string fromPath, pngPath;
    for (int i = 4; i + 1 < argc; i += 2) {
        const std::string flag = argv[i];
        if (flag == "--from") fromPath = argv[i + 1];
        else if (flag == "--png") pngPath = argv[i + 1];
        else { std::fprintf(stderr, "unknown option %s\n", flag.c_str()); return 2; }
    }

    std::vector<uint32_t> rom;
    if (!host::loadBinary(romPath, rom)) {
        std::fprintf(stderr, "cannot read %s\n", romPath.c_str());
        return 2;
    }

    host::MemoryPool pool;
    pool.allocate(1, uint32_t(rom.size()), /*withFramebuffers=*/true);
    pool.bind();
    host::installBios(pool.bios);
    std::copy(rom.begin(), rom.end(), pool.rom.begin());
    pool.io[REG_KEYINPUT >> 2] = 0x03FF;
    g_flags = FLAG_RENDER;

    GbaState st{};
    host::hleBoot(st);

    if (!fromPath.empty()) {
        std::ifstream f(fromPath, std::ios::binary);
        f.read(reinterpret_cast<char*>(&st), sizeof(GbaState));
        if (!f || !getWords(f, pool.ewram) || !getWords(f, pool.iwram) || !getWords(f, pool.vram) ||
            !getWords(f, pool.pram) || !getWords(f, pool.oam) || !getWords(f, pool.io) ||
            !getWords(f, pool.sram)) {
            std::fprintf(stderr, "cannot load %s\n", fromPath.c_str());
            return 2;
        }
        st.inst = 0;
    }

    std::ifstream script(scriptPath);
    if (!script) {
        std::fprintf(stderr, "cannot read %s\n", scriptPath.c_str());
        return 2;
    }

    uint64_t frames = 0;
    const auto run = [&](uint32_t mask, uint32_t count) {
        const uint32_t key = 0x03FFu & ~mask;  // active low
        for (uint32_t i = 0; i < count; ++i, ++frames) {
            pool.input[0] = key;
            io_set16(st, REG_KEYINPUT, key);
            step_cycles(st, CYCLES_PER_FRAME);
        }
    };

    std::string line;
    int lineNo = 0;
    while (std::getline(script, line)) {
        ++lineNo;
        if (const auto hash = line.find('#'); hash != std::string::npos) line.resize(hash);
        std::stringstream ss(line);
        std::string first;
        if (!(ss >> first)) continue;

        uint32_t mask = 0;
        if (first == "mash") {
            std::string buttons;
            uint32_t count = 0;
            if (!(ss >> buttons >> count) || !parseButtons(buttons, mask)) {
                std::fprintf(stderr, "%s:%d: expected 'mash <buttons> <count>'\n", scriptPath.c_str(), lineNo);
                return 2;
            }
            for (uint32_t i = 0; i < count; ++i) { run(mask, 4); run(0, 4); }
            continue;
        }
        uint32_t count = 0;
        if (!parseButtons(first, mask) || !(ss >> count)) {
            std::fprintf(stderr, "%s:%d: expected '<buttons> <frames> [xN]'\n", scriptPath.c_str(), lineNo);
            return 2;
        }
        uint32_t repeat = 1;
        std::string rep;
        if (ss >> rep) {
            if (rep.size() < 2 || rep[0] != 'x') {
                std::fprintf(stderr, "%s:%d: repeat must look like x10\n", scriptPath.c_str(), lineNo);
                return 2;
            }
            repeat = uint32_t(std::strtoul(rep.c_str() + 1, nullptr, 10));
        }
        for (uint32_t r = 0; r < repeat; ++r) run(mask, count);
    }

    std::ofstream f(outPath, std::ios::binary);
    f.write(reinterpret_cast<const char*>(&st), sizeof(GbaState));
    putWords(f, pool.ewram); putWords(f, pool.iwram); putWords(f, pool.vram); putWords(f, pool.pram);
    putWords(f, pool.oam); putWords(f, pool.io); putWords(f, pool.sram);
    if (!f.good()) {
        std::fprintf(stderr, "cannot write %s\n", outPath.c_str());
        return 1;
    }
    std::printf("ran %llu frames (%.1f s of game time), wrote %s\n", (unsigned long long)frames,
                double(frames) / 60.0, outPath.c_str());

    if (!pngPath.empty()) {
        const std::vector<uint32_t> fb(pool.fb.begin(), pool.fb.begin() + FB_WORDS);
        host::writePng(pngPath, host::bgr555ToRgb(fb, SCREEN_W, SCREEN_H), SCREEN_W, SCREEN_H);
        std::printf("wrote %s\n", pngPath.c_str());
    }
    return 0;
}
