// M1 gate: exercise every memory region at 8-, 16- and 32-bit width, including
// mirroring, open bus, read-only regions, the video-memory byte-write quirks,
// the 8-bit save bus, and per-instance isolation.
//
// Instance isolation is tested because every access in the emulator goes
// through MEM_IDX, and a bug there would let instances silently corrupt each
// other -- which at 4096 instances is essentially undebuggable. Better to
// catch it here against two instances.

#include "host/memory.h"

#include <cstdio>
#include <string>

using namespace gba;

namespace {

int g_failures = 0;
int g_checks = 0;

void checkEq(const char* what, uint32_t got, uint32_t want) {
    ++g_checks;
    if (got == want) return;
    ++g_failures;
    std::printf("  FAIL %-46s got 0x%08X want 0x%08X\n", what, got, want);
}

// Every region that is plain read/write memory behaves the same way, so the
// basic width and mirroring checks are driven from a table.
struct Region {
    const char* name;
    uint32_t base;
    uint32_t mirrorStride;  // 0 = do not test mirroring
};

void testReadWriteRegion(GbaState& st, const Region& rg) {
    std::printf("%s @ 0x%08X\n", rg.name, rg.base);

    bus_write32(st, rg.base + 0x40u, 0xDEADBEEFu);
    checkEq("read32 after write32", bus_read32(st, rg.base + 0x40u), 0xDEADBEEFu);
    checkEq("read16 low half", bus_read16(st, rg.base + 0x40u), 0xBEEFu);
    checkEq("read16 high half", bus_read16(st, rg.base + 0x42u), 0xDEADu);
    checkEq("read8 byte 0", bus_read8(st, rg.base + 0x40u), 0xEFu);
    checkEq("read8 byte 3", bus_read8(st, rg.base + 0x43u), 0xDEu);

    // A halfword store must leave the neighbouring halfword untouched.
    bus_write16(st, rg.base + 0x42u, 0x1234u);
    checkEq("write16 high half only", bus_read32(st, rg.base + 0x40u), 0x1234BEEFu);

    // Unaligned accesses are masked down by the bus; the rotation LDR applies
    // is CPU behaviour and is handled in M2, not here.
    checkEq("read32 ignores low addr bits", bus_read32(st, rg.base + 0x43u), 0x1234BEEFu);

    if (rg.mirrorStride) {
        bus_write32(st, rg.base + 0x80u, 0xCAFEF00Du);
        checkEq("mirror aliases base", bus_read32(st, rg.base + rg.mirrorStride + 0x80u),
                0xCAFEF00Du);
        bus_write32(st, rg.base + rg.mirrorStride + 0x80u, 0x5A5A5A5Au);
        checkEq("write through mirror", bus_read32(st, rg.base + 0x80u), 0x5A5A5A5Au);
    }
}

}  // namespace

int main() {
    host::MemoryPool pool;
    pool.allocate(/*instances=*/2, /*romWords=*/0x1000u, /*withFramebuffers=*/false);
    pool.bind();

    // A recognisable ROM image so cartridge reads can be distinguished from
    // open-bus reads.
    for (uint32_t i = 0; i < pool.rom.size(); ++i) pool.rom[i] = 0xA0000000u + i;

    GbaState st{};
    st.inst = 0;

    const Region regions[] = {
        {"EWRAM", 0x02000000u, EWRAM_SIZE},
        {"IWRAM", 0x03000000u, IWRAM_SIZE},
        {"PRAM ", 0x05000000u, PRAM_SIZE},
        {"VRAM ", 0x06000000u, 0x00020000u},  // 96 KiB region, 128 KiB window
        {"OAM  ", 0x07000000u, OAM_SIZE},
        {"IO   ", 0x04000000u, 0u},
    };
    for (const auto& rg : regions) testReadWriteRegion(st, rg);

    std::printf("VRAM non-power-of-two mirror\n");
    // 0x06018000 aliases 0x06010000, not 0x06000000. This is the mirror that
    // is easy to get wrong and corrupts OBJ tiles when you do.
    bus_write32(st, 0x06010000u, 0x11112222u);
    checkEq("0x06018000 aliases 0x06010000", bus_read32(st, 0x06018000u), 0x11112222u);
    bus_write32(st, 0x06000000u, 0x33334444u);
    checkEq("0x06018000 is NOT 0x06000000", bus_read32(st, 0x06018000u), 0x11112222u);

    std::printf("cartridge\n");
    checkEq("ROM read at 0x08000000", bus_read32(st, 0x08000000u), 0xA0000000u);
    checkEq("ROM read at 0x08000004", bus_read32(st, 0x08000004u), 0xA0000001u);
    checkEq("ROM mirrored at 0x0A000000", bus_read32(st, 0x0A000000u), 0xA0000000u);
    checkEq("ROM mirrored at 0x0C000000", bus_read32(st, 0x0C000000u), 0xA0000000u);
    bus_write32(st, 0x08000000u, 0u);
    checkEq("ROM ignores writes", bus_read32(st, 0x08000000u), 0xA0000000u);
    // Past the end of the cartridge the bus returns consecutive halfwords of
    // (addr >> 1). At 0x08100000: 0x08100000>>1 = 0x04080000, whose low
    // halfword is 0x0000, so the word reads back as 0x0001_0000.
    checkEq("ROM out of bounds", bus_read32(st, 0x08100000u), 0x00010000u);
    checkEq("ROM out of bounds (odd)", bus_read32(st, 0x08100004u), 0x00030002u);

    std::printf("BIOS\n");
    pool.bios[0] = 0xB105B105u;
    checkEq("BIOS read", bus_read32(st, 0x00000000u), 0xB105B105u);
    bus_write32(st, 0x00000000u, 0u);
    checkEq("BIOS ignores writes", bus_read32(st, 0x00000000u), 0xB105B105u);

    std::printf("open bus\n");
    bus_read32(st, 0x03000040u);  // drives a known value onto the bus
    const uint32_t lastOnBus = st.open_bus;
    checkEq("unmapped region 0x01 returns open bus", bus_read32(st, 0x01000000u), lastOnBus);
    checkEq("above BIOS returns open bus", bus_read32(st, 0x00004000u), lastOnBus);

    std::printf("video byte-write quirks\n");
    bus_write32(st, 0x07000000u, 0x00000000u);
    bus_write8(st, 0x07000000u, 0xFFu);
    checkEq("OAM drops 8-bit writes", bus_read32(st, 0x07000000u), 0x00000000u);

    bus_write32(st, 0x05000000u, 0x00000000u);
    bus_write8(st, 0x05000000u, 0xABu);
    checkEq("PRAM 8-bit write doubles byte", bus_read16(st, 0x05000000u), 0xABABu);

    // DISPCNT mode 0 -> OBJ VRAM starts at 0x10000.
    bus_write16(st, 0x04000000u, 0x0000u);
    bus_write32(st, 0x06000000u, 0x00000000u);
    bus_write8(st, 0x06000000u, 0xCDu);
    checkEq("VRAM BG 8-bit write doubles byte", bus_read16(st, 0x06000000u), 0xCDCDu);
    bus_write32(st, 0x06010000u, 0x00000000u);
    bus_write8(st, 0x06010000u, 0xCDu);
    checkEq("VRAM OBJ drops 8-bit writes (mode 0)", bus_read16(st, 0x06010000u), 0x0000u);
    // Mode 3 moves the OBJ boundary up to 0x14000, so the same address is now BG.
    bus_write16(st, 0x04000000u, 0x0003u);
    bus_write8(st, 0x06010000u, 0xCDu);
    checkEq("VRAM 0x10000 is BG in mode 3", bus_read16(st, 0x06010000u), 0xCDCDu);

    std::printf("save memory (8-bit bus)\n");
    bus_write8(st, 0x0E000000u, 0x5Au);
    checkEq("save read8", bus_read8(st, 0x0E000000u), 0x5Au);
    checkEq("save read32 replicates byte", bus_read32(st, 0x0E000000u), 0x5A5A5A5Au);
    checkEq("save read16 replicates byte", bus_read16(st, 0x0E000000u), 0x5A5Au);
    bus_write32(st, 0x0E000004u, 0x11223344u);
    checkEq("save write32 stores low byte only", bus_read8(st, 0x0E000004u), 0x44u);

    std::printf("per-instance isolation\n");
    GbaState st1{};
    st1.inst = 1;
    bus_write32(st, 0x02000100u, 0xAAAAAAAAu);
    bus_write32(st1, 0x02000100u, 0xBBBBBBBBu);
    checkEq("instance 0 EWRAM unaffected", bus_read32(st, 0x02000100u), 0xAAAAAAAAu);
    checkEq("instance 1 EWRAM independent", bus_read32(st1, 0x02000100u), 0xBBBBBBBBu);
    bus_write32(st, 0x06000200u, 0xCCCCCCCCu);
    bus_write32(st1, 0x06000200u, 0xDDDDDDDDu);
    checkEq("instance 0 VRAM unaffected", bus_read32(st, 0x06000200u), 0xCCCCCCCCu);
    checkEq("instance 1 VRAM independent", bus_read32(st1, 0x06000200u), 0xDDDDDDDDu);

    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    if (g_failures) {
        std::printf("M1 FAILED\n");
        return 1;
    }
    std::printf("M1 PASS: memory bus behaves\n");
    return 0;
}
