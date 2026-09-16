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

layout(push_constant) uniform PushBlock {
    uint g_num_instances;
    uint g_rom_words;   // ROM length in words; reads past it return open bus
    uint g_cycles;      // cycles to run this dispatch
    uint g_flags;
} pc;
#define g_num_instances pc.g_num_instances
#define g_rom_words     pc.g_rom_words

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

extern U32 g_num_instances;
extern U32 g_rom_words;

#endif

// Index of word `w` of instance `inst` in a region `words_per_inst` long.
//
// M4-M7 use this array-of-structures form: each instance's region is
// contiguous. It is simple and cache-hostile -- 32 lanes of a SIMD group touch
// 32 addresses 256 KiB apart, so no two share a cache line.
//
// M8 replaces the body with the interleaved form
//     ((w) * g_num_instances + (inst))
// so lanes hit consecutive words and coalesce. Every memory access in the
// emulator goes through this macro precisely so that is a one-line change.
#define MEM_IDX(words_per_inst, inst, w) ((inst) * (words_per_inst) + (w))

#endif  // GBA_CORE_STATE_H
