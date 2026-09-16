#ifndef GBA_CORE_MEMMAP_H
#define GBA_CORE_MEMMAP_H

#include "types.h"

// GBA memory map. Sizes are in bytes; the _WORDS variants are what actually
// index the uint[] storage arrays.

KCONST U32 BIOS_SIZE  = 0x00004000u;  //  16 KiB, shared read-only
KCONST U32 EWRAM_SIZE = 0x00040000u;  // 256 KiB, per instance
KCONST U32 IWRAM_SIZE = 0x00008000u;  //  32 KiB, per instance
KCONST U32 IO_SIZE    = 0x00000400u;  //   1 KiB, per instance
KCONST U32 PRAM_SIZE  = 0x00000400u;  //   1 KiB, per instance
KCONST U32 VRAM_SIZE  = 0x00018000u;  //  96 KiB, per instance
KCONST U32 OAM_SIZE   = 0x00000400u;  //   1 KiB, per instance
KCONST U32 SRAM_SIZE  = 0x00020000u;  // 128 KiB, per instance (Flash worst case)

KCONST U32 BIOS_WORDS  = BIOS_SIZE  >> 2u;
KCONST U32 EWRAM_WORDS = EWRAM_SIZE >> 2u;
KCONST U32 IWRAM_WORDS = IWRAM_SIZE >> 2u;
KCONST U32 IO_WORDS    = IO_SIZE    >> 2u;
KCONST U32 PRAM_WORDS  = PRAM_SIZE  >> 2u;
KCONST U32 VRAM_WORDS  = VRAM_SIZE  >> 2u;
KCONST U32 OAM_WORDS   = OAM_SIZE   >> 2u;
KCONST U32 SRAM_WORDS  = SRAM_SIZE  >> 2u;

// Simple power-of-two mirrors. EWRAM repeats every 256 KiB across 0x02xxxxxx,
// IWRAM every 32 KiB across 0x03xxxxxx, and PRAM/OAM every 1 KiB.
KCONST U32 EWRAM_MASK = EWRAM_SIZE - 1u;
KCONST U32 IWRAM_MASK = IWRAM_SIZE - 1u;
KCONST U32 IO_MASK    = IO_SIZE - 1u;
KCONST U32 PRAM_MASK  = PRAM_SIZE - 1u;
KCONST U32 OAM_MASK   = OAM_SIZE - 1u;

// Screen.
KCONST U32 SCREEN_W = 240u;
KCONST U32 SCREEN_H = 160u;
KCONST U32 FB_WORDS = (SCREEN_W * SCREEN_H) >> 1u;  // 2 bytes/pixel -> 2 px/word

// Observation buffer: the framebuffer downsampled 4x and reduced to 8-bit
// grayscale, which is what a reinforcement-learning workload actually wants.
// A full framebuffer is 75 KiB per instance; this is 2.4 KiB, so reading back
// 4096 of them per frame is 9.6 MiB rather than 300 MiB.
// GBA_OBS_SHIFT picks the downsample: 0 gives the native 240x160, 1 gives
// 120x80, 2 gives 60x40. Every option divides both screen dimensions exactly,
// so the aspect ratio is preserved. Set it in CMake with -DGBA_OBS_SHIFT=N; the
// shader and the host must agree, so it is passed to both from one place.
//
// The default is 1. Measured, the observation size costs the emulator nothing
// at any of these -- throughput is identical because emulation dominates so
// completely -- so the choice is only about what the network can afford.
// 120x80 is 9,600 pixels against the 7,056 of the 84x84 that Atari agents use.
#ifndef GBA_OBS_SHIFT
#define GBA_OBS_SHIFT 1
#endif

KCONST U32 OBS_SCALE = 1u << GBA_OBS_SHIFT;
KCONST U32 OBS_W = SCREEN_W >> GBA_OBS_SHIFT;
KCONST U32 OBS_H = SCREEN_H >> GBA_OBS_SHIFT;
KCONST U32 OBS_WORDS = (OBS_W * OBS_H) / 4u;  // four 8-bit samples per word

// Timing. One frame is 228 scanlines of 1232 cycles = 280,896 cycles at
// 16.78 MHz. Referenced by the dispatch loop, which runs whole scanlines.
KCONST U32 CYCLES_PER_SCANLINE = 1232u;
KCONST U32 SCANLINES_PER_FRAME = 228u;
KCONST U32 CYCLES_PER_FRAME    = CYCLES_PER_SCANLINE * SCANLINES_PER_FRAME;

// VRAM does not mirror on a power of two: it is 96 KiB repeating in 128 KiB
// windows, and inside each window the last 32 KiB aliases the 64..96 KiB range.
// Getting this wrong corrupts OBJ tiles in a way that only shows up in games,
// never in CPU tests, so it is worth having in exactly one place.
CORE_FN U32 vram_offset(U32 addr) {
    U32 off = addr & 0x0001FFFFu;
    if (off >= VRAM_SIZE) off = off - 0x00008000u;
    return off;
}

// CPU mode encodings in CPSR[4:0], and the register-bank index each maps to.
KCONST U32 MODE_USR = 0x10u;
KCONST U32 MODE_FIQ = 0x11u;
KCONST U32 MODE_IRQ = 0x12u;
KCONST U32 MODE_SVC = 0x13u;
KCONST U32 MODE_ABT = 0x17u;
KCONST U32 MODE_UND = 0x1Bu;
KCONST U32 MODE_SYS = 0x1Fu;

KCONST U32 BANK_USR = 0u;
KCONST U32 BANK_FIQ = 1u;
KCONST U32 BANK_IRQ = 2u;
KCONST U32 BANK_SVC = 3u;
KCONST U32 BANK_ABT = 4u;
KCONST U32 BANK_UND = 5u;
KCONST U32 NUM_BANKS = 6u;

CORE_FN U32 mode_to_bank(U32 mode) {
    switch (mode & 0x1Fu) {
        case MODE_FIQ: return BANK_FIQ;
        case MODE_IRQ: return BANK_IRQ;
        case MODE_SVC: return BANK_SVC;
        case MODE_ABT: return BANK_ABT;
        case MODE_UND: return BANK_UND;
        default:       return BANK_USR;  // USR and SYS share the user bank
    }
}

// CPSR bits.
KCONST U32 CPSR_T = 0x00000020u;  // Thumb
KCONST U32 CPSR_F = 0x00000040u;  // FIQ disable
KCONST U32 CPSR_I = 0x00000080u;  // IRQ disable
KCONST U32 CPSR_V = 0x10000000u;
KCONST U32 CPSR_C = 0x20000000u;
KCONST U32 CPSR_Z = 0x40000000u;
KCONST U32 CPSR_N = 0x80000000u;

#endif  // GBA_CORE_MEMMAP_H
