#include "host/memory.h"

#include <cstdio>
#include <fstream>

// Definitions for the globals the core declares extern. On the GPU these are
// storage buffer bindings; the core's code is identical either way.
U32* g_bios = nullptr;
U32* g_rom = nullptr;
U32* g_ewram = nullptr;
U32* g_iwram = nullptr;
U32* g_vram = nullptr;
U32* g_pram = nullptr;
U32* g_oam = nullptr;
U32* g_io = nullptr;
U32* g_sram = nullptr;
U32* g_fb = nullptr;

U32 g_num_instances = 1;
U32 g_rom_words = 0;
U32 g_flags = 0;

namespace gba::host {

void MemoryPool::allocate(uint32_t instances, uint32_t romWords, bool withFramebuffers) {
    numInstances = instances;

    bios.assign(BIOS_WORDS, 0u);
    rom.assign(romWords ? romWords : 1u, 0u);
    ewram.assign(size_t(EWRAM_WORDS) * instances, 0u);
    iwram.assign(size_t(IWRAM_WORDS) * instances, 0u);
    vram.assign(size_t(VRAM_WORDS) * instances, 0u);
    pram.assign(size_t(PRAM_WORDS) * instances, 0u);
    oam.assign(size_t(OAM_WORDS) * instances, 0u);
    io.assign(size_t(IO_WORDS) * instances, 0u);
    sram.assign(size_t(SRAM_WORDS) * instances, 0xFFFFFFFFu);  // erased flash
    fb.assign(withFramebuffers ? size_t(FB_WORDS) * instances : 1u, 0u);
}

void MemoryPool::bind() {
    g_bios = bios.data();
    g_rom = rom.data();
    g_ewram = ewram.data();
    g_iwram = iwram.data();
    g_vram = vram.data();
    g_pram = pram.data();
    g_oam = oam.data();
    g_io = io.data();
    g_sram = sram.data();
    g_fb = fb.data();
    g_num_instances = numInstances;
    g_rom_words = uint32_t(rom.size());
}

bool loadBinary(const std::string& path, std::vector<uint32_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto bytes = static_cast<size_t>(f.tellg());
    out.assign((bytes + 3) / 4, 0u);
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(bytes));
    return true;
}

void installBios(std::vector<uint32_t>& bios) {
    bios.assign(BIOS_WORDS, 0u);

    // Vector table. Reset is never taken (hleBoot jumps straight to the
    // cartridge); the unused vectors return immediately rather than running off
    // into zeros, which makes a stray exception survivable and debuggable.
    constexpr uint32_t kMovsPcLr = 0xE1B0F00Eu;  // MOVS pc, lr
    bios[0x00 / 4] = 0xEA00000Eu;                // B 0x40
    bios[0x04 / 4] = kMovsPcLr;                  // undefined instruction
    bios[0x08 / 4] = kMovsPcLr;                  // SWI: unhandled ones just return
    bios[0x0C / 4] = kMovsPcLr;                  // prefetch abort
    bios[0x10 / 4] = kMovsPcLr;                  // data abort
    bios[0x14 / 4] = kMovsPcLr;                  // reserved
    bios[0x18 / 4] = 0xEA000008u;                // B 0x40 -- IRQ
    bios[0x1C / 4] = kMovsPcLr;                  // FIQ

    // The stock IRQ handler, at 0x40. It saves the registers the ARM calling
    // convention treats as scratch, then jumps through the pointer the game
    // stored at 0x03007FFC (reached here as 0x03FFFFFC, an IWRAM mirror).
    static const uint32_t kIrqHandler[] = {
        0xE92D500Fu,  // STMFD sp!, {r0-r3, r12, lr}
        0xE3A00404u,  // MOV   r0, #0x04000000
        0xE28FE000u,  // ADD   lr, pc, #0          return address for the handler
        0xE510F004u,  // LDR   pc, [r0, #-4]       jump to [0x03FFFFFC]
        0xE8BD500Fu,  // LDMFD sp!, {r0-r3, r12, lr}
        0xE25EF004u,  // SUBS  pc, lr, #4          return, restoring CPSR
    };
    for (size_t i = 0; i < sizeof(kIrqHandler) / sizeof(kIrqHandler[0]); ++i)
        bios[0x40 / 4 + i] = kIrqHandler[i];
}

void hleBoot(GbaState& st) {
    // Values the real BIOS leaves behind before jumping to the cartridge. The
    // three stack pointers matter most: a ROM that takes an interrupt or a SWI
    // with an unset banked SP corrupts memory in confusing ways.
    for (U32 i = 0; i < 16u; ++i) st.r[i] = 0u;
    for (U32 i = 0; i < NUM_BANKS; ++i) {
        st.spsr[i] = 0u;
        st.bank_r13[i] = 0u;
        st.bank_r14[i] = 0u;
    }
    for (U32 i = 0; i < 5u; ++i) { st.bank_fiq[i] = 0u; st.bank_usr[i] = 0u; }

    st.bank_r13[BANK_SVC] = 0x03007FE0u;
    st.bank_r13[BANK_IRQ] = 0x03007FA0u;
    st.bank_r13[BANK_USR] = 0x03007F00u;

    st.r[13] = 0x03007F00u;   // we boot in SYS mode, which uses the user bank
    st.r[15] = 0x08000000u;   // cartridge entry point
    st.cpsr = MODE_SYS;       // ARM state, interrupts enabled
    st.open_bus = 0u;
    st.cycles = 0u;
    st.halted = 0u;
    st.scanline = 0u;
    st.line_cycle = 0u;
    st.dma_enabled = 0u;
    for (U32 i = 0u; i < 4u; ++i) {
        st.dma_src[i] = 0u;
        st.dma_dst[i] = 0u;
        st.dma_count[i] = 0u;
        st.timer_counter[i] = 0u;
        st.timer_reload[i] = 0u;
        st.timer_prescale[i] = 0u;
    }
}

}  // namespace gba::host
