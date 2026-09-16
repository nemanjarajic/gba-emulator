// M2/M3 gate: run a jsmolka CPU test ROM on the reference core.
//
// The ROM keeps its verdict in r12: zero once every test has passed, otherwise
// the number of the first test that failed. After the last test it waits for
// vblank, draws its result, and drops into `b idle` -- a branch to itself. That
// self-branch is the signal that the run is over, and it is what this harness
// watches for, so no knowledge of ROM addresses is needed.

#include "core/core.inc"
#include "host/memory.h"

#include <algorithm>
#include <cstdio>
#include <utility>
#include <cstdlib>
#include <string>
#include <vector>

using namespace gba;

namespace {

// Generous: the ROM draws its result text one character at a time.
constexpr uint64_t kMaxInstructions = 200ull * 1000 * 1000;

}  // namespace

int main(int argc, char** argv) {
    const std::string romPath =
        argc > 1 ? argv[1] : "third_party/gba-tests/arm/arm.gba";

    std::vector<uint32_t> rom;
    if (!host::loadBinary(romPath, rom)) {
        std::fprintf(stderr, "cannot read %s\n", romPath.c_str());
        return 2;
    }

    host::MemoryPool pool;
    pool.allocate(/*instances=*/1, uint32_t(rom.size()), /*withFramebuffers=*/true);
    pool.bind();
    host::installBios(pool.bios);
    pool.io[REG_KEYINPUT >> 2] = 0x03FF;  // KEYINPUT is active low: no keys held
    std::copy(rom.begin(), rom.end(), pool.rom.begin());

    GbaState st{};
    st.inst = 0;
    host::hleBoot(st);

    std::printf("running %s (%zu KiB)\n", romPath.c_str(), rom.size() * 4 / 1024);

    // Ring buffer of recent instructions. A runaway PC is the usual failure
    // mode for a young interpreter, and the useful information is the handful
    // of instructions before it left the rails, not the address it ended at.
    constexpr size_t kTrace = 24;
    struct Entry { uint32_t pc, op, cpsr, r12; bool thumb; };
    std::vector<Entry> trace(kTrace);
    size_t traceAt = 0;

    // The BIOS region now holds a real vector table and IRQ handler, so
    // executing there is legitimate.
    auto isCode = [](uint32_t pc) {
        const uint32_t region = (pc >> 24) & 0xF;
        return region == 0x0 || region == 0x2 || region == 0x3 || region == 0x8 || region == 0x9;
    };

    // Branch history. A runaway PC is almost always the tail of a control-flow
    // path that went wrong earlier, and the list of jumps is far more legible
    // than the instruction stream.
    constexpr size_t kBranches = 32;
    struct Branch { uint32_t from, to; bool thumb; };
    std::vector<Branch> branches(kBranches);
    size_t branchAt = 0;

    auto dumpBranches = [&]() {
        std::printf("  last %zu branches taken:\n", kBranches);
        for (size_t i = 0; i < kBranches; ++i) {
            const Branch& b = branches[(branchAt + i) % kBranches];
            if (b.from == 0 && b.to == 0) continue;
            std::printf("    %s %08X -> %08X\n", b.thumb ? "T" : "A", b.from, b.to);
        }
    };

    auto dumpState = [&]() {
        std::printf("  registers:\n");
        for (int i = 0; i < 16; i += 4)
            std::printf("    r%-2d %08X  r%-2d %08X  r%-2d %08X  r%-2d %08X\n", i, st.r[i], i + 1,
                        st.r[i + 1], i + 2, st.r[i + 2], i + 3, st.r[i + 3]);
        std::printf("    cpsr %08X  mode %02X  %s\n", st.cpsr, st.cpsr & 0x1F,
                    (st.cpsr & CPSR_T) ? "Thumb" : "ARM");
        // After an exception r13 is the new mode's banked stack pointer, which
        // was never initialised; the interesting one is the user/system bank.
        const uint32_t sp = st.bank_r13[BANK_USR];
        std::printf("    user-bank sp %08X\n", sp);
        std::printf("  stack around sp=%08X:\n", sp);
        for (int i = -4; i < 8; ++i) {
            const uint32_t a = sp + uint32_t(i * 4);
            std::printf("    [sp%+3d] %08X: %08X\n", i * 4, a, bus_read32(st, a));
        }
    };

    auto dumpTrace = [&](const char* why) {
        std::printf("%s\n  last %zu instructions:\n", why, kTrace);
        for (size_t i = 0; i < kTrace; ++i) {
            const Entry& e = trace[(traceAt + i) % kTrace];
            if (e.pc == 0 && e.op == 0) continue;
            std::printf("    %s 0x%08X: %0*X   cpsr=%08X r12=%u\n", e.thumb ? "T" : "A", e.pc,
                        e.thumb ? 4 : 8, e.op, e.cpsr, e.r12);
        }
    };

    // Coverage instrumentation.
    //
    // r12 alone is a weak signal: the m_exit macro only writes it when a test
    // FAILS, so "r12 == 0" looks identical whether every test passed or none
    // ever ran. Counting the distinct ROM words executed distinguishes the two
    // -- a harness that stopped early would show a fraction of the test code
    // touched. The ARM/Thumb split confirms both decoders are live.
    std::vector<bool> executedWord(rom.size(), false);
    uint64_t armCount = 0, thumbCount = 0;

    // Coarse PC histogram, bucketed by 256 bytes. When a game runs but does
    // nothing visible, the question is which code it is actually spending its
    // frames in, and a sampled histogram answers that in one run.
    const bool histogram = getenv("HIST") != nullptr;
    std::vector<uint32_t> hist;
    if (histogram) hist.assign(1u << 16, 0u);  // 16 MiB of ROM / 256

    const bool logBranches = getenv("BRANCHES") != nullptr;
    // Start logging only after the boot sequence, so the steady-state main loop
    // can be read without wading through initialisation.
    const char* fromEnv = getenv("BRANCH_FROM");
    const uint64_t branchFrom = fromEnv ? strtoull(fromEnv, nullptr, 0) : 0;
    // Optional memory watchpoint: WATCH=0x03007E24 reports every instruction
    // that changes that word. Far quicker than reasoning backwards from a
    // corrupted stack slot.
    const char* watchEnv = getenv("WATCH");
    const uint32_t watchAddr = watchEnv ? uint32_t(strtoul(watchEnv, nullptr, 0)) : 0;
    uint32_t watchPrev = 0;
    const char* tracePcEnv = getenv("TRACEPC");
    const uint32_t tracePc = tracePcEnv ? uint32_t(strtoul(tracePcEnv, nullptr, 0)) : 0;
    const uint32_t traceSpan = 0x20;
    uint64_t executed = 0;
    bool finished = false;
    for (; executed < kMaxInstructions; ++executed) {
        const uint32_t pc = st.r[15];
        const bool thumb = (st.cpsr & CPSR_T) != 0;

        // Mirror step_cycles: interrupts are dispatched between instructions,
        // and a halted CPU still advances the clock so the interrupt that wakes
        // it can arrive. Calling cpu_step alone would never take an interrupt.
        irq_check(st);
        if (st.halted) {
            const uint32_t toLineEnd = CYCLES_PER_SCANLINE - st.line_cycle;
            st.cycles += toLineEnd;
            scheduler_tick(st, toLineEnd);
            continue;
        }

        trace[traceAt] = {pc, thumb ? bus_read16(st, pc) : bus_read32(st, pc), st.cpsr, st.r[12],
                          thumb};
        traceAt = (traceAt + 1) % kTrace;

        if (histogram && (pc >> 24) == 0x08) ++hist[(pc & 0x00FFFFFF) >> 8];

        if (thumb) ++thumbCount; else ++armCount;
        if ((pc >> 24) == 0x08) {
            const size_t w = (pc & 0x01FFFFFF) >> 2;
            if (w < executedWord.size()) executedWord[w] = true;
        }

        if (tracePc && pc >= tracePc && pc <= tracePc + traceSpan)
            std::printf("TR %08X: %0*X  r0=%08X r1=%08X r2=%08X r3=%08X r4=%08X r5=%08X\n", pc,
                        thumb ? 4 : 8, thumb ? bus_read16(st, pc) : bus_read32(st, pc), st.r[0],
                        st.r[1], st.r[2], st.r[3], st.r[4], st.r[5]);

        cpu_step(st);

        if (watchAddr) {
            const uint32_t now = bus_read32(st, watchAddr);
            if (now != watchPrev) {
                std::printf("WATCH %6llu  [%08X] %08X -> %08X   by %s %08X: %0*X\n",
                            (unsigned long long)executed, watchAddr, watchPrev, now,
                            thumb ? "T" : "A", pc, thumb ? 4 : 8,
                            thumb ? bus_read16(st, pc) : bus_read32(st, pc));
                watchPrev = now;
            }
        }

        if (st.r[15] != pc + (thumb ? 2u : 4u)) {
            branches[branchAt] = {pc, st.r[15], thumb};
            branchAt = (branchAt + 1) % kBranches;
            if (logBranches && executed >= branchFrom)
                std::printf("BR %6llu  %s %08X -> %08X   lr=%08X sp=%08X\n",
                            (unsigned long long)executed, thumb ? "T" : "A", pc, st.r[15],
                            st.r[14], st.r[13]);
        }

        if (st.halted) {
            dumpTrace("CPU halted");
            return 1;
        }
        if (!isCode(st.r[15])) {
            std::printf("PC left executable memory: 0x%08X after %llu instructions\n", st.r[15],
                        (unsigned long long)executed);
            dumpBranches();
            dumpState();
            return 1;
        }
        if (st.r[15] == pc) {  // branch to self: the ROM is done
            finished = true;
            std::printf("self-branch at 0x%08X after %llu instructions\n", pc,
                        (unsigned long long)executed);
            if (getenv("TRACE")) dumpTrace("");
            break;
        }
    }

    if (histogram) {
        std::vector<std::pair<uint32_t, uint32_t>> top;
        for (uint32_t i = 0; i < hist.size(); ++i)
            if (hist[i]) top.push_back({hist[i], i});
        std::sort(top.rbegin(), top.rend());
        std::printf("hottest ROM regions (%zu distinct 256-byte blocks executed):\n", top.size());
        for (size_t i = 0; i < top.size() && i < 12; ++i)
            std::printf("  0x%08X  %10u instructions (%.1f%%)\n", 0x08000000 + (top[i].second << 8),
                        top[i].first, 100.0 * top[i].first / double(executed));
    }

    if (!finished) {
        std::printf("FAILED: no result after %llu instructions (PC 0x%08X)\n",
                    (unsigned long long)executed, st.r[15]);
        return 1;
    }

    const uint32_t verdict = st.r[12];
    std::printf("stopped after %llu instructions (%llu ARM, %llu Thumb)\n",
                (unsigned long long)executed, (unsigned long long)armCount,
                (unsigned long long)thumbCount);
    size_t covered = 0;
    for (bool b : executedWord) covered += b ? 1 : 0;
    std::printf("ROM coverage: %zu/%zu words executed (%.0f%%), final r12 = %u\n", covered,
                rom.size(), 100.0 * double(covered) / double(rom.size()), verdict);

    if (verdict != 0) {
        std::printf("FAILED: first failing test is #%u\n", verdict);
        std::printf("  (test numbering: conditions 1+, branches 50+, flags 100+,\n"
                    "   shifts 150+, data processing 200+, psr transfer 250+,\n"
                    "   multiply 300+, single transfer 350+, halfword transfer 400+,\n"
                    "   data swap 450+, block transfer 500+)\n");
        return 1;
    }
    std::printf("PASS: %s reports all tests passed\n", romPath.c_str());
    return 0;
}
