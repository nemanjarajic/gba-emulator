// An interactive player and debugger for a single GBA instance.
//
// This runs on the CPU reference core, not the GPU, and that is not a
// compromise: one instance on the GPU manages 0.33 MHz against the 16.78 MHz a
// GBA needs, while the CPU core does 96 Mcycle/s -- about six times real time.
// The GPU path exists to run thousands of machines at once, and cannot run one
// of them at playable speed. See docs/performance.md.
//
// The entire interface is drawn into a pixel buffer, so `--png <file>` renders
// it without a display. That is how the layout gets checked.

#include "core/core.inc"
#include "host/canvas.h"
#include "host/disasm.h"
#include "host/memory.h"
#include "host/png.h"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace gba;
using namespace gba::host;

namespace {

constexpr int kScale = 3;
constexpr int kScreenX = 10, kScreenY = 30;
constexpr int kPanelX = kScreenX + int(SCREEN_W) * kScale + 12;
constexpr int kPanelW = 356;
constexpr int kWinW = kPanelX + kPanelW + 10;
constexpr int kWinH = kScreenY + int(SCREEN_H) * kScale + 54;

std::string hex32(uint32_t v) {
    char b[16];
    std::snprintf(b, sizeof b, "%08X", v);
    return b;
}
std::string hex8(uint32_t v) {
    char b[8];
    std::snprintf(b, sizeof b, "%02X", v);
    return b;
}

// Everything the debugger needs to keep between frames.
struct Session {
    MemoryPool pool;
    GbaState st{};
    std::vector<uint32_t> rom;
    std::string romName;

    bool running = false;
    bool quit = false;
    uint64_t instructions = 0;
    uint32_t frames = 0;
    double fps = 0.0;
    uint32_t keyinput = 0x03FF;  // active low

    uint32_t prevRegs[16] = {};
    std::vector<uint32_t> breakpoints;
    const char* stopReason = "paused at reset";

    // Memory view: a handful of useful places rather than a text entry box.
    int memView = 0;
    static constexpr uint32_t kMemBases[4] = {0x03000000, 0x02000000, 0x04000000, 0x06000000};
    static constexpr const char* kMemNames[4] = {"IWRAM", "EWRAM", "I/O", "VRAM"};

    // Save state. A snapshot is the machine's registers plus its memory, and
    // for one instance that is just a copy of the pool's vectors.
    bool haveSnapshot = false;
    GbaState snapState{};
    std::vector<uint32_t> snapEwram, snapIwram, snapVram, snapPram, snapOam, snapIo, snapSram;

    bool atBreakpoint(uint32_t pc) const {
        for (uint32_t b : breakpoints)
            if (b == pc) return true;
        return false;
    }
};

void saveState(Session& s) {
    s.snapState = s.st;
    s.snapEwram = s.pool.ewram;
    s.snapIwram = s.pool.iwram;
    s.snapVram = s.pool.vram;
    s.snapPram = s.pool.pram;
    s.snapOam = s.pool.oam;
    s.snapIo = s.pool.io;
    s.snapSram = s.pool.sram;
    s.haveSnapshot = true;
}

void loadState(Session& s) {
    if (!s.haveSnapshot) return;
    s.st = s.snapState;
    s.pool.ewram = s.snapEwram;
    s.pool.iwram = s.snapIwram;
    s.pool.vram = s.snapVram;
    s.pool.pram = s.snapPram;
    s.pool.oam = s.snapOam;
    s.pool.io = s.snapIo;
    s.pool.sram = s.snapSram;
    s.pool.bind();
}

// Advances one instruction, mirroring step_cycles so interrupts are taken and
// a halted CPU still advances the clock.
void stepOne(Session& s) {
    irq_check(s.st);
    if (s.st.halted) {
        const uint32_t skip = CYCLES_PER_SCANLINE - s.st.line_cycle;
        s.st.cycles += skip;
        scheduler_tick(s.st, skip);
        return;
    }
    std::memcpy(s.prevRegs, s.st.r, sizeof s.prevRegs);
    cpu_step(s.st);
    ++s.instructions;
}

// Runs until the frame ends, a breakpoint is hit, or the budget runs out.
void runFrame(Session& s) {
    const uint32_t target = s.st.cycles + CYCLES_PER_FRAME;
    while (s.st.cycles < target) {
        if (!s.breakpoints.empty() && s.atBreakpoint(s.st.r[15])) {
            s.running = false;
            s.stopReason = "breakpoint";
            return;
        }
        stepOne(s);
    }
    ++s.frames;
}

void drawPanel(Canvas& c, int x, int y, int w, int h, const char* title) {
    c.rect(x, y, w, h, kPanel);
    c.frame(x, y, w, h, {0x2C, 0x31, 0x3A});
    c.text(x + 6, y + 5, title, kDim);
}

void render(Canvas& c, Session& s) {
    c.clear(kBg);

    // --- header -------------------------------------------------------------
    c.text(10, 8, s.romName, kText);
    const std::string status = s.running ? "RUNNING" : std::string("PAUSED  ") + s.stopReason;
    c.text(kPanelX, 8, status, s.running ? kChanged : kWarn);
    char rate[64];
    std::snprintf(rate, sizeof rate, "%5.1f fps   frame %u", s.fps, s.frames);
    c.text(kWinW - 10 - int(std::strlen(rate)) * 6, 8, rate, kDim);

    // --- the screen ---------------------------------------------------------
    c.frame(kScreenX - 1, kScreenY - 1, int(SCREEN_W) * kScale + 2, int(SCREEN_H) * kScale + 2,
            {0x2C, 0x31, 0x3A});
    c.blitFramebuffer(kScreenX, kScreenY, s.pool.fb, SCREEN_W, SCREEN_H, kScale);

    // --- registers ----------------------------------------------------------
    int y = kScreenY;
    drawPanel(c, kPanelX, y, kPanelW, 136, "REGISTERS");
    for (int i = 0; i < 16; ++i) {
        const int col = i / 8, row = i % 8;
        const int rx = kPanelX + 8 + col * 176;
        const int ry = y + 20 + row * 12;
        static const char* names[16] = {"r0 ", "r1 ", "r2 ", "r3 ", "r4 ", "r5 ", "r6 ", "r7 ",
                                        "r8 ", "r9 ", "r10", "r11", "r12", "sp ", "lr ", "pc "};
        c.text(rx, ry, names[i], kDim);
        // Green marks a register the last instruction changed.
        c.text(rx + 24, ry, hex32(s.st.r[i]),
               s.st.r[i] != s.prevRegs[i] ? kChanged : kText);
    }
    const uint32_t mode = s.st.cpsr & 0x1F;
    const char* mn = mode == 0x10   ? "USR"
                     : mode == 0x11 ? "FIQ"
                     : mode == 0x12 ? "IRQ"
                     : mode == 0x13 ? "SVC"
                     : mode == 0x17 ? "ABT"
                     : mode == 0x1B ? "UND"
                     : mode == 0x1F ? "SYS"
                                    : "???";
    std::string flags;
    flags += (s.st.cpsr & CPSR_N) ? 'N' : '-';
    flags += (s.st.cpsr & CPSR_Z) ? 'Z' : '-';
    flags += (s.st.cpsr & CPSR_C) ? 'C' : '-';
    flags += (s.st.cpsr & CPSR_V) ? 'V' : '-';
    flags += (s.st.cpsr & CPSR_I) ? 'I' : '-';
    c.text(kPanelX + 8, y + 118,
           "cpsr " + hex32(s.st.cpsr) + "  " + flags + "  " + mn + "  " +
               ((s.st.cpsr & CPSR_T) ? "Thumb" : "ARM") + (s.st.halted ? "  HALT" : ""),
           kAccent);

    // --- disassembly --------------------------------------------------------
    y += 144;
    drawPanel(c, kPanelX, y, kPanelW, 194, "DISASSEMBLY");
    const bool thumb = (s.st.cpsr & CPSR_T) != 0;
    const uint32_t step = thumb ? 2 : 4;
    uint32_t addr = s.st.r[15] - step * 4;
    for (int i = 0; i < 15; ++i, addr += step) {
        const int ly = y + 20 + i * 11;
        const bool here = addr == s.st.r[15];
        if (here) c.rect(kPanelX + 4, ly - 2, kPanelW - 8, 11, {0x24, 0x32, 0x4A});
        if (s.atBreakpoint(addr)) c.rect(kPanelX + 4, ly - 2, 3, 11, {0xD0, 0x4A, 0x4A});
        const uint32_t op = thumb ? bus_read16(s.st, addr) : bus_read32(s.st, addr);
        const std::string text = thumb ? disasmThumb(uint16_t(op), addr) : disasmArm(op, addr);
        c.text(kPanelX + 12, ly, hex32(addr), here ? kAccent : kDim);
        c.text(kPanelX + 68, ly, thumb ? hex8(op >> 8) + hex8(op & 0xFF) : hex32(op), kDim);
        c.text(kPanelX + 68 + (thumb ? 30 : 54), ly, text, here ? kText : kDim);
    }

    // --- memory -------------------------------------------------------------
    y += 202;
    const uint32_t base = Session::kMemBases[s.memView];
    drawPanel(c, kPanelX, y, kPanelW, 104,
              (std::string("MEMORY  ") + Session::kMemNames[s.memView]).c_str());
    for (int row = 0; row < 7; ++row) {
        const uint32_t a = base + uint32_t(row) * 8;
        std::string line = hex32(a) + " ";
        std::string ascii = " ";
        for (int i = 0; i < 8; ++i) {
            const uint32_t byte = bus_read8(s.st, a + uint32_t(i));
            line += " " + hex8(byte);
            ascii += (byte >= 32 && byte < 127) ? char(byte) : '.';
        }
        c.text(kPanelX + 8, y + 20 + row * 11, line, kDim);
        c.text(kPanelX + 8 + int(line.size()) * 6, y + 20 + row * 11, ascii, kText);
    }

    // --- help ---------------------------------------------------------------
    c.text(10, kWinH - 24,
           "space run/pause   n step   f frame   b breakpoint   c clear   m memory   "
           "F5/F9 save/load", kDim);
    c.text(10, kWinH - 13,
           "buttons: Z=B  X=A  enter=start  rshift=select  arrows=dpad  q=L  w=R   esc quit",
           kDim);
}

// Keyboard to KEYINPUT. The register is active low, so a held key clears a bit.
uint32_t readButtons(const bool* keys) {
    uint32_t v = 0x03FF;
    auto held = [&](SDL_Scancode sc, uint32_t bit) {
        if (keys[sc]) v &= ~bit;
    };
    held(SDL_SCANCODE_X, 1u << 0);       // A
    held(SDL_SCANCODE_Z, 1u << 1);       // B
    held(SDL_SCANCODE_RSHIFT, 1u << 2);  // Select
    held(SDL_SCANCODE_RETURN, 1u << 3);  // Start
    held(SDL_SCANCODE_RIGHT, 1u << 4);
    held(SDL_SCANCODE_LEFT, 1u << 5);
    held(SDL_SCANCODE_UP, 1u << 6);
    held(SDL_SCANCODE_DOWN, 1u << 7);
    held(SDL_SCANCODE_W, 1u << 8);  // R
    held(SDL_SCANCODE_Q, 1u << 9);  // L
    return v;
}

}  // namespace

int main(int argc, char** argv) {
    std::string romPath;
    std::string pngOut;
    uint32_t pngFrames = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--png" && i + 1 < argc) pngOut = argv[++i];
        else if (a == "--frames" && i + 1 < argc) pngFrames = uint32_t(std::atoi(argv[++i]));
        else romPath = a;
    }
    if (romPath.empty()) {
        std::fprintf(stderr,
                     "usage: debugger <rom.gba> [--png out.png --frames N]\n"
                     "  --png renders the interface to a file without opening a window\n");
        return 2;
    }

    Session s;
    if (!loadBinary(romPath, s.rom)) {
        std::fprintf(stderr, "cannot read %s\n", romPath.c_str());
        return 2;
    }
    s.romName = romPath.substr(romPath.find_last_of("/\\") + 1);
    s.pool.allocate(1, uint32_t(s.rom.size()), /*withFramebuffers=*/true);
    s.pool.bind();
    installBios(s.pool.bios);
    std::copy(s.rom.begin(), s.rom.end(), s.pool.rom.begin());
    hleBoot(s.st);
    g_flags = FLAG_RENDER;
    io_set16(s.st, REG_KEYINPUT, 0x03FF);

    // Headless: run a while, render the interface once, write it out.
    if (!pngOut.empty()) {
        for (uint32_t f = 0; f < pngFrames; ++f) runFrame(s);
        s.stopReason = "rendered headless";
        Canvas c(kWinW, kWinH);
        render(c, s);
        if (!writePng(pngOut, c.pixels(), c.width(), c.height())) {
            std::fprintf(stderr, "cannot write %s\n", pngOut.c_str());
            return 1;
        }
        std::printf("wrote %s (%dx%d) after %u frames, pc=%08X\n", pngOut.c_str(), kWinW, kWinH,
                    pngFrames, s.st.r[15]);
        return 0;
    }

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* win = SDL_CreateWindow("gba-gpu debugger", kWinW, kWinH, 0);
    SDL_Renderer* ren = SDL_CreateRenderer(win, nullptr);
    SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING,
                                         kWinW, kWinH);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

    Canvas canvas(kWinW, kWinH);
    auto lastFps = std::chrono::steady_clock::now();
    uint32_t framesThisSecond = 0;

    while (!s.quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) s.quit = true;
            if (e.type != SDL_EVENT_KEY_DOWN) continue;
            switch (e.key.scancode) {
                case SDL_SCANCODE_ESCAPE: s.quit = true; break;
                case SDL_SCANCODE_SPACE:
                    s.running = !s.running;
                    s.stopReason = "by hand";
                    break;
                case SDL_SCANCODE_N:
                    s.running = false;
                    stepOne(s);
                    s.stopReason = "stepped";
                    break;
                case SDL_SCANCODE_F:
                    s.running = false;
                    runFrame(s);
                    s.stopReason = "stepped a frame";
                    break;
                case SDL_SCANCODE_B: {
                    const uint32_t pc = s.st.r[15];
                    auto it = s.breakpoints.begin();
                    for (; it != s.breakpoints.end(); ++it)
                        if (*it == pc) break;
                    if (it == s.breakpoints.end()) s.breakpoints.push_back(pc);
                    else s.breakpoints.erase(it);
                    break;
                }
                case SDL_SCANCODE_C: s.breakpoints.clear(); break;
                case SDL_SCANCODE_M: s.memView = (s.memView + 1) % 4; break;
                case SDL_SCANCODE_F5: saveState(s); break;
                case SDL_SCANCODE_F9:
                    loadState(s);
                    s.stopReason = "state restored";
                    break;
                default: break;
            }
        }

        const bool* keys = SDL_GetKeyboardState(nullptr);
        s.keyinput = readButtons(keys);
        io_set16(s.st, REG_KEYINPUT, s.keyinput);

        if (s.running) runFrame(s);

        render(canvas, s);
        SDL_UpdateTexture(tex, nullptr, canvas.pixels().data(), kWinW * 3);
        SDL_RenderClear(ren);
        SDL_RenderTexture(ren, tex, nullptr, nullptr);
        SDL_RenderPresent(ren);

        ++framesThisSecond;
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - lastFps).count();
        if (elapsed >= 0.5) {
            s.fps = framesThisSecond / elapsed;
            framesThisSecond = 0;
            lastFps = now;
        }
        if (!s.running) SDL_Delay(16);  // idle politely while paused
    }

    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
