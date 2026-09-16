// M7 gate for the parts of the I/O hardware no ROM in the suite exercises:
// keypad interrupts, the audio FIFO DMA path, SoftReset, and IntrWait.
//
// These are driven directly against the core rather than through a ROM. They
// are logic in the shared source, so the dual-compile guarantee and m1_parity
// cover the GPU side; what needs checking here is the behaviour itself, and
// setting it up in C++ allows many more configurations than hand-assembling a
// ROM for each would.

#include "core/core.inc"
#include "host/memory.h"

#include <cstdio>
#include <vector>

using namespace gba;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

// A freshly booted machine with one instance of memory behind it.
struct Machine {
    host::MemoryPool pool;
    GbaState st{};

    Machine() {
        pool.allocate(1, 4, /*withFramebuffers=*/false);
        pool.bind();
        host::installBios(pool.bios);
        host::hleBoot(st);
        io_set16(st, REG_KEYINPUT, 0x03FFu);  // active low: nothing held
        g_flags = 0;
    }
};

}  // namespace

int main() {
    std::printf("keypad interrupts\n");
    {
        Machine m;
        // Fire when the A button (bit 0) is held; "any of these keys".
        io_set16(m.st, REG_KEYCNT, 0x4000u | 0x0001u);
        io_set16(m.st, REG_IE, IRQ_KEYPAD);
        irq_update(m.st);

        scheduler_tick(m.st, CYCLES_PER_SCANLINE);
        check((io_get16(m.st, REG_IF) & IRQ_KEYPAD) == 0, "no interrupt while nothing is held");

        io_set16(m.st, REG_KEYINPUT, 0x03FEu);  // A held
        scheduler_tick(m.st, CYCLES_PER_SCANLINE);
        check((io_get16(m.st, REG_IF) & IRQ_KEYPAD) != 0, "interrupt fires when the key is held");
    }
    {
        Machine m;
        // "All of these keys": A and B together, selected by bit 15.
        io_set16(m.st, REG_KEYCNT, 0x8000u | 0x4000u | 0x0003u);
        io_set16(m.st, REG_IE, IRQ_KEYPAD);
        irq_update(m.st);

        io_set16(m.st, REG_KEYINPUT, 0x03FEu);  // only A
        scheduler_tick(m.st, CYCLES_PER_SCANLINE);
        check((io_get16(m.st, REG_IF) & IRQ_KEYPAD) == 0, "AND condition ignores a partial match");

        io_set16(m.st, REG_KEYINPUT, 0x03FCu);  // A and B
        scheduler_tick(m.st, CYCLES_PER_SCANLINE);
        check((io_get16(m.st, REG_IF) & IRQ_KEYPAD) != 0, "AND condition fires on a full match");
    }
    {
        Machine m;
        io_set16(m.st, REG_KEYCNT, 0x0001u);  // selected, but interrupt disabled
        io_set16(m.st, REG_IE, IRQ_KEYPAD);
        irq_update(m.st);
        io_set16(m.st, REG_KEYINPUT, 0x03FEu);
        scheduler_tick(m.st, CYCLES_PER_SCANLINE);
        check((io_get16(m.st, REG_IF) & IRQ_KEYPAD) == 0,
              "no interrupt when KEYCNT's enable bit is clear");
    }

    std::printf("audio FIFO DMA\n");
    {
        Machine m;
        for (uint32_t i = 0; i < 8; ++i) bus_write32(m.st, 0x02000000u + i * 4u, 0x1000u + i);

        io_set16(m.st, REG_SOUNDCNT_H, 0u);  // timer 0 clocks both FIFOs

        // DMA1: EWRAM -> FIFO A, 32-bit, fixed destination, repeating, on the
        // "special" trigger.
        const uint32_t base = 0x040000B0u + 12u;
        bus_write32(m.st, base + 0u, 0x02000000u);
        bus_write32(m.st, base + 4u, 0x040000A0u);
        bus_write32(m.st, base + 8u, 4u | (0xB640u << 16));

        check((m.st.dma_enabled & (1u << 1)) != 0, "the channel latched when enabled");

        // Timer 0 reloads at 0xFFFF, so it overflows on the next increment.
        bus_write32(m.st, 0x04000100u, 0xFFFFu | (0x0080u << 16));
        scheduler_tick(m.st, 4u);

        // Four words went to a fixed destination, so the register holds the
        // last of them.
        check(io_get32(m.st, REG_FIFO_A) == 0x1003u, "four words reached the FIFO");
        check(m.st.dma_src[1] == 0x02000010u, "the source advanced by four words");
        check((m.st.dma_enabled & (1u << 1)) != 0, "the channel stays armed for the next overflow");

        // A second overflow continues from where the first left off.
        scheduler_tick(m.st, 0x10000u);
        check(io_get32(m.st, REG_FIFO_A) == 0x1007u, "the next overflow feeds the next four words");
    }

    std::printf("SoftReset\n");
    {
        Machine m;
        m.st.r[15] = 0x02001234u;
        cpu_set_mode(m.st, MODE_IRQ);
        bus_write32(m.st, 0x03007E00u, 0xDEADBEEFu);
        bus_write8(m.st, 0x03007FFAu, 0u);  // restart from the cartridge

        check(bios_swi(m.st, 0x00u), "SWI 0x00 is handled");
        check(m.st.r[15] == 0x08000000u, "re-enters the cartridge");
        check((m.st.cpsr & 0x1Fu) == MODE_SYS, "returns to system mode");
        check(m.st.bank_r13[BANK_IRQ] == 0x03007FA0u, "restores the interrupt stack pointer");
        check(bus_read32(m.st, 0x03007E00u) == 0u, "clears the BIOS scratch area");
    }

    std::printf("IntrWait\n");
    {
        Machine m;
        io_set16(m.st, REG_IE, IRQ_VBLANK);
        irq_update(m.st);
        // Stand in for an ARM-mode SWI at 0x08000000: r15 reads as +8 mid-execution.
        m.st.r[15] = 0x08000008u;
        m.st.pc_dirty = 0;

        // The handler already recorded a vblank, so the call must return at once.
        bus_write16(m.st, BIOS_INTR_FLAGS, IRQ_VBLANK);
        check(bios_swi(m.st, 0x05u), "VBlankIntrWait is handled");
        check(m.st.halted == 0, "returns immediately when the interrupt already arrived");
        check((bus_read16(m.st, BIOS_INTR_FLAGS) & IRQ_VBLANK) == 0, "and consumes the flag");
        check(m.st.pc_dirty == 0, "without rewinding");

        // Nothing pending: halt, and rewind so the call resumes rather than
        // returning when some unrelated interrupt wakes the CPU.
        m.st.pc_dirty = 0;
        bios_swi(m.st, 0x05u);
        check(m.st.halted == 1, "waits when the interrupt has not arrived");
        check(m.st.r[15] == 0x08000000u && m.st.pc_dirty != 0,
              "rewinds to the SWI so a different interrupt resumes the wait");
    }
    {
        Machine m;
        // Waiting on a source nothing can raise would halt forever.
        io_set16(m.st, REG_IE, 0u);
        irq_update(m.st);
        m.st.r[15] = 0x08000008u;
        m.st.r[1] = IRQ_VBLANK;
        bios_swi(m.st, 0x04u);
        check(m.st.halted == 0, "does not wait on an interrupt that is not enabled");
    }

    std::printf("\n");
    if (failures) {
        std::printf("M7 IO FAILED: %d checks failed\n", failures);
        return 1;
    }
    std::printf("M7 IO PASS\n");
    return 0;
}
