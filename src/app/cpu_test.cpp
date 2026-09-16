// M2/M3 gate: run a jsmolka CPU test ROM on the reference core.
//
// The ROM keeps its verdict in r12: zero once every test has passed, otherwise
// the number of the first test that failed. After the last test it waits for
// vblank, draws its result, and drops into `b idle` -- a branch to itself. That
// self-branch is the signal that the run is over, and it is what this harness
// watches for, so no knowledge of ROM addresses is needed.

#include "core/core.inc"
#include "host/memory.h"

#include <cstdio>
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

    // No BIOS image is loaded (the HLE BIOS handles SWIs instead), so region 0
    // contains nothing but zeros. Any jump there is a bug, and catching it on
    // entry keeps the offending instruction inside the trace window.
    auto isCode = [](uint32_t pc) {
        const uint32_t region = (pc >> 24) & 0xF;
        return region == 0x2 || region == 0x3 || region == 0x8 || region == 0x9;
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

    uint64_t executed = 0;
    bool finished = false;
    for (; executed < kMaxInstructions; ++executed) {
        const uint32_t pc = st.r[15];
        const bool thumb = (st.cpsr & CPSR_T) != 0;
        trace[traceAt] = {pc, thumb ? bus_read16(st, pc) : bus_read32(st, pc), st.cpsr, st.r[12],
                          thumb};
        traceAt = (traceAt + 1) % kTrace;

        if (thumb) ++thumbCount; else ++armCount;
        if ((pc >> 24) == 0x08) {
            const size_t w = (pc & 0x01FFFFFF) >> 2;
            if (w < executedWord.size()) executedWord[w] = true;
        }

        cpu_step(st);

        if (st.halted) {
            dumpTrace("CPU halted");
            return 1;
        }
        if (!isCode(st.r[15])) {
            std::printf("PC left executable memory: 0x%08X after %llu instructions\n", st.r[15],
                        (unsigned long long)executed);
            dumpTrace("");
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
