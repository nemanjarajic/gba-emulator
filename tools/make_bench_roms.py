#!/usr/bin/env python3
"""Generates the synthetic ROMs used by the M8 divergence benchmark.

Both ROMs steer each instance down one of 32 equal-cost code paths, chosen by a
seed the host writes to the first word of that instance's EWRAM. `m8_bench`
sets the seed to (instance % DIVERGE), so one ROM measures the whole curve.

  diverge.gba   every path uses the SAME guest instruction types, so only the
                data differs and the interpreter's decode switch does not.
  diverge2.gba  each path uses a DIFFERENT class of ARM instruction, which is
                what actually forces the emulator onto divergent branches.

Writes both to build/roms/. Run:  python3 tools/make_bench_roms.py
"""
import os
import struct
import sys

OUT = os.path.join(os.path.dirname(__file__), "..", "build", "roms")

# Each path is four words: three of work, then a branch to the common tail.
SAME_CLASS = [0xE2833000, 0xE2233000, 0xE2433000]  # ADD/EOR/SUB r3, r3, #i

MIXED_CLASSES = [
    ("data-processing register", 0xE0833003),  # ADD   r3, r3, r3
    ("data-processing immediate", 0xE2833001),  # ADD   r3, r3, #1
    ("multiply", 0xE0030391),                   # MUL   r3, r1, r3
    ("load word", 0xE5903000),                  # LDR   r3, [r0]
    ("store word", 0xE5803000),                 # STR   r3, [r0]
    ("load halfword", 0xE1D030B0),              # LDRH  r3, [r0]
    ("block transfer", 0xE8900008),             # LDMIA r0, {r3}
    ("shifted register", 0xE1A03183),           # MOV   r3, r3, LSL #3
]


def build(mixed):
    words = [
        0xE3A00402,  # 0x00 MOV r0, #0x02000000   EWRAM base
        0xE5901000,  # 0x04 LDR r1, [r0]          path index, seeded by the host
        0xE3A03001,  # 0x08 MOV r3, #1            accumulator, non-zero for MUL
        0xE3A04801,  # 0x0C MOV r4, #0x10000      outer iterations
        0xE08FF201,  # 0x10 ADD pc, pc, r1, LSL #4
        0xE1A00000,  # 0x14 padding: the jump lands at pc+8 = 0x18
    ]
    block0 = 0x18
    common = block0 + 32 * 16
    for i in range(32):
        addr = block0 + i * 16
        if mixed:
            op = MIXED_CLASSES[i % len(MIXED_CLASSES)][1]
            words += [op, op, op]
        else:
            words += [w | i for w in SAME_CLASS]
        words.append(0xEA000000 | (((common - (addr + 20)) >> 2) & 0xFFFFFF))
    assert len(words) * 4 == common, (len(words) * 4, common)
    words.append(0xE2544001)  # SUBS r4, r4, #1
    words.append(0x1A000000 | (((0x10 - (common + 12)) >> 2) & 0xFFFFFF))  # BNE outer
    words.append(0xEAFFFFFE)  # B .
    return b"".join(struct.pack("<I", w) for w in words)


def build_input_echo():
    """A ROM that publishes its controller state for the M9 harness to verify.

    Two bands are drawn every loop: rows 0-3 in a colour derived from KEYINPUT,
    and rows 4-7 in solid white. The white band matters -- a KEYINPUT value with
    few low bits set has a luminance of zero, so a screen painted only from the
    input is legitimately black and gives the harness nothing to check against.
    The controller state also goes to EWRAM word 0, which the probe path reads.
    """
    return [
        0xE3A00404,  # MOV  r0, #0x04000000
        0xE3A01B01,  # MOV  r1, #0x400
        0xE3811003,  # ORR  r1, r1, #3       DISPCNT: mode 3, BG2 on
        0xE5801000,  # STR  r1, [r0]
        0xE2803F4C,  # ADD  r3, r0, #0x130   &KEYINPUT
        0xE3A06402,  # MOV  r6, #0x02000000  EWRAM
        0xE3A07C7F,  # MOV  r7, #0x7F00
        0xE38770FF,  # ORR  r7, r7, #0xFF    r7 = 0x7FFF, white
        # loop:
        0xE1D320B0,  # LDRH r2, [r3]         read the controller
        0xE5862000,  # STR  r2, [r6]         publish for the probe
        0xE3A04406,  # MOV  r4, #0x06000000  VRAM
        0xE3A05000,  # MOV  r5, #0
        # fill1: rows 0-3 from the input
        0xE1C420B0,  # STRH r2, [r4]
        0xE2844002,  # ADD  r4, r4, #2
        0xE2855001,  # ADD  r5, r5, #1
        0xE3550E3C,  # CMP  r5, #0x3C0       960 pixels = 4 rows
        0x1AFFFFFA,  # BNE  fill1
        0xE3A05000,  # MOV  r5, #0
        # fill2: rows 4-7 solid white, a fixed reference for the harness
        0xE1C470B0,  # STRH r7, [r4]
        0xE2844002,  # ADD  r4, r4, #2
        0xE2855001,  # ADD  r5, r5, #1
        0xE3550E3C,  # CMP  r5, #0x3C0
        0x1AFFFFFA,  # BNE  fill2
        0xEAFFFFEF,  # B    loop
    ]


def main():
    os.makedirs(OUT, exist_ok=True)
    path = os.path.join(OUT, "input_echo.gba")
    data = b"".join(struct.pack("<I", w) for w in build_input_echo())
    with open(path, "wb") as f:
        f.write(data)
    print(f"wrote {os.path.normpath(path)} ({len(data)} bytes)")
    for name, mixed in (("diverge.gba", False), ("diverge2.gba", True)):
        path = os.path.join(OUT, name)
        data = build(mixed)
        with open(path, "wb") as f:
            f.write(data)
        print(f"wrote {os.path.normpath(path)} ({len(data)} bytes)")
    print("classes in diverge2.gba: " + ", ".join(n for n, _ in MIXED_CLASSES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
