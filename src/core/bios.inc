#ifndef GBA_CORE_BIOS_INC
#define GBA_CORE_BIOS_INC

#include "bus.inc"
#include "cpu.inc"
#include "io.inc"
#include "state.h"
#include "types.h"

// High-level BIOS.
//
// The GBA BIOS is copyrighted and not redistributable, so rather than requiring
// a dump this implements the SWI calls directly. An HLE call consumes the SWI
// without taking an exception, so execution continues at the next instruction.
//
// A small real vector table IS installed in the BIOS region by the host (see
// host::installBios): the interrupt path has to be genuine ARM code, because a
// game's handler is reached by the BIOS loading a pointer from 0x03007FFC.

// Signed division built from unsigned operations.
//
// `int(a) / int(b)` is not portable across our two targets: GLSL leaves % and /
// undefined when an operand is negative, while C++ defines them. Doing the sign
// handling by hand keeps both targets identical, which is the whole premise of
// the shared core.
CORE_FN void sdiv32(U32 num, U32 den, OUT(U32) quot, OUT(U32) rem) {
    bool neg_num = (num & 0x80000000u) != 0u;
    bool neg_den = (den & 0x80000000u) != 0u;
    U32 a = neg_num ? (0u - num) : num;
    U32 b = neg_den ? (0u - den) : den;
    U32 q = a / b;
    U32 r = a - q * b;
    if (neg_num != neg_den) q = 0u - q;
    if (neg_num) r = 0u - r;
    quot = q;
    rem = r;
}

CORE_FN U32 isqrt32(U32 x) {
    U32 res = 0u;
    U32 bit = 1u << 30u;
    while (bit > x) bit = bit >> 2u;
    while (bit != 0u) {
        if (x >= res + bit) {
            x = x - (res + bit);
            res = (res >> 1u) + bit;
        } else {
            res = res >> 1u;
        }
        bit = bit >> 2u;
    }
    return res;
}

// Sine of a 16-bit angle (0x10000 == a full turn), returned in Q14 so that
// +-0x4000 is +-1.0 -- the format the affine helpers want.
//
// A polynomial rather than the library sin(): GLSL's sin() and C++'s std::sin()
// are not required to agree bit for bit, and a one-ulp difference would show up
// as a CPU/GPU divergence. Integer arithmetic is identical on both.
CORE_FN I32 bios_sin(U32 angle) {
    U32 quadrant = (angle >> 14u) & 3u;
    U32 x = angle & 0x3FFFu;                 // position within the quadrant
    if ((quadrant & 1u) != 0u) x = 0x4000u - x;
    U32 z = x << 1u;                          // to Q15 over [0, 1]
    if (z > 32768u) z = 32768u;

    // sin(pi*z/2) ~= z*(A - z^2*(B - z^2*C)), the standard Q15 minimax fit.
    U32 z2 = (z * z) >> 15u;
    U32 inner = 21166u - ((z2 * 2611u) >> 15u);
    inner = 51470u - ((z2 * inner) >> 15u);
    U32 mag = (z * inner) >> 16u;             // Q15 result, halved into Q14

    return (quadrant >= 2u) ? -I32(mag) : I32(mag);
}

CORE_FN I32 bios_cos(U32 angle) { return bios_sin(angle + 0x4000u); }

// Writes one byte through a halfword access.
//
// The decompressors are allowed to target VRAM, where an 8-bit store is doubled
// across the containing halfword and would corrupt the neighbouring byte. A
// read-modify-write of the halfword is correct everywhere.
CORE_FN void bios_put8(INOUT(GbaState) st, U32 addr, U32 value) {
    U32 half_addr = addr & (~1u);
    U32 cur = bus_read16(st, half_addr);
    U32 merged = ((addr & 1u) != 0u) ? ((cur & 0x00FFu) | ((value & 0xFFu) << 8u))
                                     : ((cur & 0xFF00u) | (value & 0xFFu));
    bus_write16(st, half_addr, merged);
}

// SWI 0x0B CpuSet / SWI 0x0C CpuFastSet.
CORE_FN void bios_cpu_set(INOUT(GbaState) st, bool fast) {
    U32 src = st.r[0];
    U32 dst = st.r[1];
    U32 ctrl = st.r[2];
    U32 count = ctrl & 0x001FFFFFu;
    bool fill = ((ctrl >> 24u) & 1u) != 0u;
    bool word = fast || (((ctrl >> 26u) & 1u) != 0u);

    if (word) {
        src = src & 0xFFFFFFFCu;
        dst = dst & 0xFFFFFFFCu;
        U32 value = bus_read32(st, src);
        NO_UNROLL for (U32 i = 0u; i < count; ++i) {
            if (!fill) value = bus_read32(st, src + i * 4u);
            bus_write32(st, dst + i * 4u, value);
        }
    } else {
        src = src & 0xFFFFFFFEu;
        dst = dst & 0xFFFFFFFEu;
        U32 value = bus_read16(st, src);
        NO_UNROLL for (U32 i = 0u; i < count; ++i) {
            if (!fill) value = bus_read16(st, src + i * 2u);
            bus_write16(st, dst + i * 2u, value);
        }
    }
}

// SWI 0x01 RegisterRamReset. Each flag bit clears one area.
CORE_FN void bios_ram_reset(INOUT(GbaState) st, U32 flags) {
    if ((flags & 0x01u) != 0u) {
        // The top 0x200 bytes of EWRAM hold the BIOS's own scratch and survive.
        for (U32 i = 0u; i < EWRAM_WORDS - 128u; ++i)
            g_ewram[MEM_IDX(EWRAM_WORDS, st.inst, i)] = 0u;
    }
    if ((flags & 0x02u) != 0u) {
        for (U32 i = 0u; i < IWRAM_WORDS - 128u; ++i)
            g_iwram[MEM_IDX(IWRAM_WORDS, st.inst, i)] = 0u;
    }
    if ((flags & 0x04u) != 0u)
        for (U32 i = 0u; i < PRAM_WORDS; ++i) g_pram[MEM_IDX(PRAM_WORDS, st.inst, i)] = 0u;
    if ((flags & 0x08u) != 0u)
        for (U32 i = 0u; i < VRAM_WORDS; ++i) g_vram[MEM_IDX(VRAM_WORDS, st.inst, i)] = 0u;
    if ((flags & 0x10u) != 0u)
        for (U32 i = 0u; i < OAM_WORDS; ++i) g_oam[MEM_IDX(OAM_WORDS, st.inst, i)] = 0u;
    if ((flags & 0x80u) != 0u) {
        // Display and DMA registers back to their reset values.
        for (U32 off = 0u; off < 0x60u; off = off + 2u) io_set16(st, off, 0u);
        for (U32 off = 0xB0u; off < 0xE0u; off = off + 2u) io_set16(st, off, 0u);
        io_set16(st, 0x00u, 0x0080u);  // forced blank, as after a real reset
    }
}

// SWI 0x11/0x12 LZ77UnComp. Header word: type in bits 0-7, size in bits 8-31.
CORE_FN void bios_lz77(INOUT(GbaState) st) {
    U32 src = st.r[0];
    U32 dst = st.r[1];
    U32 header = bus_read32(st, src);
    U32 remaining = header >> 8u;
    src = src + 4u;

    NO_UNROLL while (remaining > 0u) {
        U32 flags = bus_read8(st, src);
        src = src + 1u;
        for (U32 bit = 0u; bit < 8u && remaining > 0u; ++bit) {
            if ((flags & (0x80u >> bit)) != 0u) {
                // Back-reference: length in the high nibble, distance in the
                // remaining twelve bits, both biased.
                U32 b0 = bus_read8(st, src);
                U32 b1 = bus_read8(st, src + 1u);
                src = src + 2u;
                U32 length = (b0 >> 4u) + 3u;
                U32 distance = (((b0 & 0x0Fu) << 8u) | b1) + 1u;
                for (U32 i = 0u; i < length && remaining > 0u; ++i) {
                    bios_put8(st, dst, bus_read8(st, dst - distance));
                    dst = dst + 1u;
                    remaining = remaining - 1u;
                }
            } else {
                bios_put8(st, dst, bus_read8(st, src));
                src = src + 1u;
                dst = dst + 1u;
                remaining = remaining - 1u;
            }
        }
    }
}

// SWI 0x14/0x15 RLUnComp.
CORE_FN void bios_rle(INOUT(GbaState) st) {
    U32 src = st.r[0];
    U32 dst = st.r[1];
    U32 remaining = bus_read32(st, src) >> 8u;
    src = src + 4u;

    NO_UNROLL while (remaining > 0u) {
        U32 flag = bus_read8(st, src);
        src = src + 1u;
        if ((flag & 0x80u) != 0u) {
            U32 length = (flag & 0x7Fu) + 3u;
            U32 value = bus_read8(st, src);
            src = src + 1u;
            for (U32 i = 0u; i < length && remaining > 0u; ++i) {
                bios_put8(st, dst, value);
                dst = dst + 1u;
                remaining = remaining - 1u;
            }
        } else {
            U32 length = (flag & 0x7Fu) + 1u;
            for (U32 i = 0u; i < length && remaining > 0u; ++i) {
                bios_put8(st, dst, bus_read8(st, src));
                src = src + 1u;
                dst = dst + 1u;
                remaining = remaining - 1u;
            }
        }
    }
}

// SWI 0x13 HuffUnComp. The tree is a compact array of nodes; each node byte
// holds an offset to its children plus two flags saying whether each child is
// a leaf.
CORE_FN void bios_huffman(INOUT(GbaState) st) {
    U32 src = st.r[0];
    U32 dst = st.r[1];
    U32 header = bus_read32(st, src);
    U32 remaining = header >> 8u;
    U32 data_bits = header & 0x0Fu;  // 4 or 8

    U32 tree_root = src + 5u;
    U32 stream = src + 4u + ((bus_read8(st, src + 4u) + 1u) * 2u);

    U32 node = tree_root;
    U32 node_value = bus_read8(st, node);
    U32 out_byte = 0u;
    U32 out_bits = 0u;

    NO_UNROLL while (remaining > 0u) {
        U32 word = bus_read32(st, stream);
        stream = stream + 4u;
        for (U32 bit = 0u; bit < 32u && remaining > 0u; ++bit) {
            U32 one = (word >> (31u - bit)) & 1u;
            U32 child = ((node & (~1u)) + ((node_value & 0x3Fu) + 1u) * 2u) + one;
            bool leaf = (one != 0u) ? ((node_value & 0x40u) != 0u) : ((node_value & 0x80u) != 0u);
            node = child;
            node_value = bus_read8(st, node);

            if (!leaf) continue;

            if (data_bits == 8u) {
                bios_put8(st, dst, node_value);
                dst = dst + 1u;
                remaining = remaining - 1u;
            } else {
                // Four-bit symbols pack two to a byte, low nibble first.
                out_byte = out_byte | ((node_value & 0x0Fu) << out_bits);
                out_bits = out_bits + 4u;
                if (out_bits == 8u) {
                    bios_put8(st, dst, out_byte);
                    dst = dst + 1u;
                    remaining = remaining - 1u;
                    out_byte = 0u;
                    out_bits = 0u;
                }
            }
            node = tree_root;
            node_value = bus_read8(st, node);
        }
    }
}

// SWI 0x10 BitUnPack: widens packed 1/2/4/8-bit source values.
CORE_FN void bios_bit_unpack(INOUT(GbaState) st) {
    U32 src = st.r[0];
    U32 dst = st.r[1];
    U32 params = st.r[2];

    U32 src_len = bus_read16(st, params);
    U32 src_width = bus_read8(st, params + 2u);
    U32 dst_width = bus_read8(st, params + 3u);
    U32 offset_word = bus_read32(st, params + 4u);
    U32 offset = offset_word & 0x7FFFFFFFu;
    bool offset_zero = (offset_word & 0x80000000u) != 0u;

    U32 acc = 0u;
    U32 bits = 0u;
    U32 src_mask = (1u << src_width) - 1u;

    NO_UNROLL for (U32 i = 0u; i < src_len; ++i) {
        U32 byte = bus_read8(st, src + i);
        for (U32 s = 0u; s < 8u; s = s + src_width) {
            U32 value = (byte >> s) & src_mask;
            if (value != 0u || offset_zero) value = value + offset;
            acc = acc | (value << bits);
            bits = bits + dst_width;
            if (bits == 32u) {
                bus_write32(st, dst, acc);
                dst = dst + 4u;
                acc = 0u;
                bits = 0u;
            }
        }
    }
}

// SWI 0x0F ObjAffineSet: builds sprite matrices from an angle and scale.
CORE_FN void bios_obj_affine_set(INOUT(GbaState) st) {
    U32 src = st.r[0];
    U32 dst = st.r[1];
    U32 count = st.r[2];
    U32 stride = st.r[3];

    for (U32 i = 0u; i < count; ++i) {
        I32 sx = I32(sign_extend(bus_read16(st, src + 0u), 16u));
        I32 sy = I32(sign_extend(bus_read16(st, src + 2u), 16u));
        U32 angle = bus_read16(st, src + 4u) & 0xFFFFu;
        src = src + 8u;

        I32 c = bios_cos(angle);
        I32 s = bios_sin(angle);
        bus_write16(st, dst + 0u * stride, U32((sx * c) >> 14));
        bus_write16(st, dst + 1u * stride, U32((-sx * s) >> 14));
        bus_write16(st, dst + 2u * stride, U32((sy * s) >> 14));
        bus_write16(st, dst + 3u * stride, U32((sy * c) >> 14));
        dst = dst + 4u * stride;
    }
}

// SWI 0x0E BgAffineSet: the same, plus the reference-point maths.
CORE_FN void bios_bg_affine_set(INOUT(GbaState) st) {
    U32 src = st.r[0];
    U32 dst = st.r[1];
    U32 count = st.r[2];

    for (U32 i = 0u; i < count; ++i) {
        I32 cx = I32(bus_read32(st, src + 0u));
        I32 cy = I32(bus_read32(st, src + 4u));
        I32 dx = I32(sign_extend(bus_read16(st, src + 8u), 16u));
        I32 dy = I32(sign_extend(bus_read16(st, src + 10u), 16u));
        I32 sx = I32(sign_extend(bus_read16(st, src + 12u), 16u));
        I32 sy = I32(sign_extend(bus_read16(st, src + 14u), 16u));
        U32 angle = bus_read16(st, src + 16u) & 0xFFFFu;
        src = src + 20u;

        I32 c = bios_cos(angle);
        I32 s = bios_sin(angle);
        I32 pa = (sx * c) >> 14;
        I32 pb = (-sx * s) >> 14;
        I32 pc = (sy * s) >> 14;
        I32 pd = (sy * c) >> 14;

        bus_write16(st, dst + 0u, U32(pa));
        bus_write16(st, dst + 2u, U32(pb));
        bus_write16(st, dst + 4u, U32(pc));
        bus_write16(st, dst + 6u, U32(pd));
        bus_write32(st, dst + 8u, U32(cx - pa * dx - pb * dy));
        bus_write32(st, dst + 12u, U32(cy - pc * dx - pd * dy));
        dst = dst + 16u;
    }
}

// Returns true if the call was handled. Unhandled numbers fall through to a
// real SWI exception, which the installed vector table turns into a return.
CORE_FN bool bios_swi(INOUT(GbaState) st, U32 number) {
    switch (number) {
        case 0x01u: bios_ram_reset(st, st.r[0]); return true;

        case 0x02u:  // Halt
            st.halted = 1u;
            return true;

        case 0x04u:  // IntrWait
        case 0x05u:  // VBlankIntrWait
            // Modelled as "halt until any enabled interrupt". The real call
            // loops until one of the specific sources in r1 arrives; games
            // invariably re-check their own flag afterwards, so returning early
            // costs a spin rather than correctness.
            if (number == 0x05u) io_set16(st, REG_IE, io_get16(st, REG_IE) | IRQ_VBLANK);
            st.halted = 1u;
            return true;

        case 0x06u: {  // Div: r0 / r1
            if (st.r[1] == 0u) return true;  // hardware is undefined here
            U32 q, r;
            sdiv32(st.r[0], st.r[1], q, r);
            st.r[0] = q;
            st.r[1] = r;
            st.r[3] = (q & 0x80000000u) != 0u ? (0u - q) : q;  // abs(quotient)
            return true;
        }
        case 0x07u: {  // DivArm: operands reversed
            if (st.r[0] == 0u) return true;
            U32 q, r;
            sdiv32(st.r[1], st.r[0], q, r);
            st.r[0] = q;
            st.r[1] = r;
            st.r[3] = (q & 0x80000000u) != 0u ? (0u - q) : q;
            return true;
        }
        case 0x08u: st.r[0] = isqrt32(st.r[0]); return true;  // Sqrt

        case 0x0Bu: bios_cpu_set(st, false); return true;     // CpuSet
        case 0x0Cu: bios_cpu_set(st, true); return true;      // CpuFastSet

        case 0x0Eu: bios_bg_affine_set(st); return true;
        case 0x0Fu: bios_obj_affine_set(st); return true;
        case 0x10u: bios_bit_unpack(st); return true;
        case 0x11u: case 0x12u: bios_lz77(st); return true;
        case 0x13u: bios_huffman(st); return true;
        case 0x14u: case 0x15u: bios_rle(st); return true;

        default:
            // SoftReset, the multiplayer and sound-driver calls, and the
            // diff-filter decompressors are not implemented.
            return false;
    }
}

#endif  // GBA_CORE_BIOS_INC
