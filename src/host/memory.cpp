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
}

}  // namespace gba::host
