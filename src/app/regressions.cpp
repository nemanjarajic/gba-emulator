// Regression gate: known-answer checks for bugs the test ROMs did not catch.
//
// The parity harnesses only prove the CPU and GPU builds agree, and a decoding
// bug in the shared core makes them agree on the wrong answer. Each check here
// pins down one such bug with the value real hardware produces.

#include "core/core.inc"
#include "host/memory.h"

#include <cstdio>

using namespace gba;

namespace {

int g_failures = 0;
int g_checks = 0;

void checkEq(const char* what, uint32_t got, uint32_t want) {
    ++g_checks;
    if (got == want) return;
    ++g_failures;
    std::printf("  FAIL %-52s got 0x%08X want 0x%08X\n", what, got, want);
}

// Thumb format 8 is 0101 H S 1 Ro Rb Rd, and bits 11:10 select
// 0 STRH, 1 LDSB, 2 LDRH, 3 LDSH. LDSB and LDRH were swapped, and jsmolka's
// thumb.gba passed regardless; Pokemon Emerald crashed entering its first map.
void testThumbFormat8(GbaState& st) {
    std::printf("Thumb format 8 (register-offset halfword and signed loads)\n");
    const uint32_t base = 0x02000000u;
    const auto op = [](uint32_t kind) {  // Ro = r2, Rb = r1, Rd = r0
        return 0x5200u | (kind << 10) | (2u << 6) | (1u << 3) | 0u;
    };
    bus_write32(st, base + 4u, 0x000080F0u);
    st.r[1] = base;
    st.r[2] = 4u;

    thumb_execute(st, op(1u));
    checkEq("LDSB sign-extends the byte", st.r[0], 0xFFFFFFF0u);
    thumb_execute(st, op(2u));
    checkEq("LDRH zero-extends the halfword", st.r[0], 0x000080F0u);
    thumb_execute(st, op(3u));
    checkEq("LDSH sign-extends the halfword", st.r[0], 0xFFFF80F0u);
    st.r[0] = 0x1234BEEFu;
    thumb_execute(st, op(0u));
    checkEq("STRH stores only the low halfword", bus_read32(st, base + 4u), 0x0000BEEFu);
}

// The PPU reads VRAM by offset, and offsets computed from tile numbers run past
// 96 KiB. They must mirror as the bus does (0x18000 onto 0x10000), not index
// past the instance's slice -- into the next instance, or past the buffer.
void testPpuVramMirror(GbaState& st, host::MemoryPool& pool) {
    std::printf("PPU VRAM reads mirror like the bus\n");
    bus_write32(st, 0x06010000u, 0x44332211u);
    // Instance 1's VRAM starts where instance 0's unmirrored offset would land.
    pool.vram[VRAM_WORDS + ((0x18000u - VRAM_SIZE) >> 2)] = 0xEEEEEEEEu;

    checkEq("offset 0x18000 reads 0x10000", vram_read8_raw(st, 0x18000u), 0x11u);
    checkEq("offset 0x18002 reads 0x10002 (16-bit)", vram_read16_raw(st, 0x18002u), 0x4433u);
    checkEq("raw read agrees with the bus at 0x1FFFF", vram_read8_raw(st, 0x1FFFFu),
            bus_read8(st, 0x0601FFFFu));
}

}  // namespace

int main() {
    host::MemoryPool pool;
    pool.allocate(/*instances=*/2, /*romWords=*/1u, /*withFramebuffers=*/false);
    pool.bind();

    GbaState st{};
    st.inst = 0;
    st.cpsr = MODE_SYS | CPSR_T;

    testThumbFormat8(st);
    testPpuVramMirror(st, pool);

    if (g_failures) {
        std::printf("REGRESSIONS FAIL: %d of %d checks\n", g_failures, g_checks);
        return 1;
    }
    std::printf("REGRESSIONS PASS: %d checks\n", g_checks);
    return 0;
}
