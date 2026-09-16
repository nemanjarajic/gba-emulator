#ifndef GBA_CORE_STATE_H
#define GBA_CORE_STATE_H

#include "memmap.h"
#include "types.h"

// Per-instance machine state, minus the bulk memory regions.
//
// This struct holds only what is small and hot: the CPU register file, the
// scheduler, and bus housekeeping. The large regions (EWRAM, IWRAM, VRAM,
// PRAM, OAM, I/O, save, framebuffer) live in their own storage buffers and are
// reached through the accessors in bus.inc, indexed by `inst`.
//
// Splitting it this way is what lets the GPU core keep the register file in
// shader locals for the length of a dispatch instead of round-tripping to
// memory on every instruction.
//
// All members are U32 with no initialisers, both GLSL requirements. Zero one
// with `GbaState st{};` on the C++ side.
struct GbaState {
    U32 inst;  // instance index; selects this machine's slice of every region

    // --- CPU register file -------------------------------------------------
    // r[] is the *active* bank. Switching mode swaps registers between r[] and
    // the bank_ arrays below, so the interpreter never pays for an indirection.
    U32 r[16];
    U32 cpsr;
    U32 spsr[NUM_BANKS];

    // r8-r12 have a dedicated FIQ bank; r13/r14 are banked for all six modes.
    // Only one of these two is "live" at a time: while outside FIQ mode the FIQ
    // copies of r8-r12 park in bank_fiq, and while inside FIQ mode the user
    // copies park in bank_usr.
    U32 bank_fiq[5];
    U32 bank_usr[5];
    U32 bank_r13[NUM_BANKS];
    U32 bank_r14[NUM_BANKS];

    // --- bus ---------------------------------------------------------------
    // Reads from unmapped addresses return whatever was last driven on the
    // bus rather than zero. Games do rely on this.
    U32 open_bus;

    // Set whenever an instruction writes r15, so the step loop knows not to
    // advance the PC itself. A flag rather than a comparison because
    // `MOV r15, r15` legitimately writes back the value the PC already holds.
    U32 pc_dirty;

    // --- DMA ---------------------------------------------------------------
    // Latched copies of the source, destination and count. Hardware snapshots
    // these when a channel is enabled, so later writes to the registers do not
    // disturb a transfer already in flight.
    U32 dma_src[4];
    U32 dma_dst[4];
    U32 dma_count[4];
    U32 dma_enabled;  // bitmask: channels whose registers have been latched

    // --- timers ------------------------------------------------------------
    // TMxCNT_L reads as the live counter but writes the reload value, so the
    // two cannot share the I/O word.
    U32 timer_counter[4];
    U32 timer_reload[4];
    U32 timer_active;  // bitmask of enabled timers, so the common case exits early

    // Cached (IE & IF) != 0. The interrupt check runs between every pair of
    // instructions, and reading the I/O registers there cost an uncoalesced
    // memory access per instruction on the GPU.
    U32 irq_ready;
    U32 timer_prescale[4];  // cycles accumulated towards the next increment

    // --- save media --------------------------------------------------------
    // Flash is a command-driven device, not a RAM array: the game writes a
    // magic sequence to unlock each operation, and reads a chip ID to decide
    // whether a cartridge has save memory at all.
    U32 flash_phase;   // position in the AA/55/command unlock sequence
    U32 flash_id_mode; // reads return the chip ID rather than data
    U32 flash_bank;    // 128 KiB parts are two banks of 64 KiB
    U32 flash_erase;   // an erase command has been unlocked

    // --- scheduler ---------------------------------------------------------
    U32 cycles;      // cycles consumed within the current dispatch
    U32 halted;      // set by the HALT BIOS call; cleared by an interrupt
    U32 scanline;    // current VCOUNT
    U32 line_cycle;  // cycles elapsed within the current scanline
};

// ---------------------------------------------------------------------------
// Storage regions.
//
// In GLSL these are std430 storage buffers; in C++ they are plain global
// arrays. Both languages see the same names with the same element type, so the
// shared code in bus.inc indexes `g_ewram[...]` identically in each.
//
// Binding order here is also the descriptor binding order the host uses.

#ifdef GBA_GLSL

layout(std430, binding = 0) readonly buffer BiosBuf  { uint g_bios[];  };
layout(std430, binding = 1) readonly buffer RomBuf   { uint g_rom[];   };
layout(std430, binding = 2) buffer EwramBuf { uint g_ewram[]; };
layout(std430, binding = 3) buffer IwramBuf { uint g_iwram[]; };
layout(std430, binding = 4) buffer VramBuf  { uint g_vram[];  };
layout(std430, binding = 5) buffer PramBuf  { uint g_pram[];  };
layout(std430, binding = 6) buffer OamBuf   { uint g_oam[];   };
layout(std430, binding = 7) buffer IoBuf    { uint g_io[];    };
layout(std430, binding = 8) buffer SramBuf  { uint g_sram[];  };
layout(std430, binding = 9) buffer FbBuf    { uint g_fb[];    };

// Per-instance machine state, persisting across dispatches. This is what makes
// the GPU core resumable: a dispatch loads its instance's state into shader
// locals, runs a bounded number of cycles, and stores it back.
//
// Declared for the shader only. The C++ reference core passes a GbaState by
// reference instead, since it runs one machine at a time.
layout(std430, binding = 10) buffer StateBuf { GbaState g_state[]; };

// One word per instance holding its KEYINPUT value, applied at the start of
// every dispatch. A separate buffer rather than writing the I/O region
// directly, so the host uploads one contiguous array per frame instead of
// scattering thousands of small writes across instance slices.
layout(std430, binding = 11) readonly buffer InputBuf { uint g_input[]; };

// Downsampled grayscale observations, written when FLAG_OBSERVE is set.
layout(std430, binding = 12) buffer ObsBuf { uint g_obs[]; };

layout(push_constant) uniform PushBlock {
    uint g_num_instances;
    uint g_rom_words;   // ROM length in words; reads past it return open bus
    uint g_cycles;      // cycles to run this dispatch
    uint g_flags;
} pc;
#define g_num_instances pc.g_num_instances
#define g_rom_words     pc.g_rom_words
#define g_cycles        pc.g_cycles
#define g_flags         pc.g_flags

#else  // ---- C++ ----

extern U32* g_bios;
extern U32* g_rom;
extern U32* g_ewram;
extern U32* g_iwram;
extern U32* g_vram;
extern U32* g_pram;
extern U32* g_oam;
extern U32* g_io;
extern U32* g_sram;
extern U32* g_fb;

extern U32* g_input;
extern U32* g_obs;

extern U32 g_num_instances;
extern U32 g_rom_words;
extern U32 g_flags;

#endif

// Bits in the push-constant `g_flags` word.
//
// Rendering is opt-in because a framebuffer costs 75 KiB per instance -- more
// than VRAM, IWRAM, PRAM, OAM and I/O combined. A throughput workload usually
// wants a reward signal, not four thousand pictures.
KCONST U32 FLAG_RENDER = 1u;
// Write a downsampled observation at the end of the dispatch. Implies
// FLAG_RENDER, since there is nothing to downsample otherwise.
KCONST U32 FLAG_OBSERVE = 2u;

// Index of word `w` of instance `inst` in a region `words_per_inst` long.
//
// Two layouts, selected at build time by GBA_SOA:
//
//   array-of-structures (default): each instance's region is contiguous.
//     Simple, and cache-hostile -- 32 lanes of a SIMD group touch 32 addresses
//     256 KiB apart, so no two ever share a cache line.
//
//   interleaved (GBA_SOA): word w of every instance is stored together.
//     Lanes that are executing the same instruction touch consecutive words and
//     coalesce into one transaction.
//
// Every memory access in the emulator goes through this macro, which is what
// makes the choice a one-line change. Host code must use the helpers in
// InstancePool rather than assuming an instance owns a contiguous slice.
#ifdef GBA_SOA
#define MEM_IDX(words_per_inst, inst, w) ((w) * g_num_instances + (inst))
#else
#define MEM_IDX(words_per_inst, inst, w) ((inst) * (words_per_inst) + (w))
#endif

// Observations are addressed contiguously per instance regardless of the
// emulator's memory layout: they are an I/O product, not emulated machine
// state, and keeping them contiguous makes reading one instance's observation
// a single download rather than a strided gather.
#define OBS_IDX(inst, w) ((inst) * OBS_WORDS + (w))

#ifndef GBA_GLSL
// GbaState is copied verbatim between host memory and a std430 storage buffer,
// so its C++ layout must match what GLSL sees. Every member being a U32 or an
// array of U32 makes that true (std430 gives both a 4-byte stride); this
// catches a member of any other type being added later.
static_assert(sizeof(GbaState) == 83u * 4u, "GbaState must stay std430-compatible");
#endif

#endif  // GBA_CORE_STATE_H
