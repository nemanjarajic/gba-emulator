# gba-gpu

A Game Boy Advance emulator where the **entire console runs inside a Vulkan
compute shader** — ARM7TDMI CPU, memory bus, PPU, DMA, timers and interrupts —
with one GPU invocation per emulated console, so thousands of independent GBA
instances run at once.

Targets macOS (Apple Silicon via MoltenVK) and Windows with an NVIDIA GPU.
Buffer allocation tries memory types in preference order and falls back to a
staging path, so it does not depend on unified memory; macOS-only Vulkan
extensions are queried before use. Priority is learning, so the code favours
legibility and visible incremental milestones over maximum compatibility.

## What this is and is not

A **single** instance runs *slower* on the GPU than on a CPU, likely by 20–50×:
a GPU lane has no branch predictor and no speculative execution, and an
ARM7TDMI interpreter loop is nothing but unpredictable branches and random
memory access. All the value is in aggregate throughput across many instances
— RL training, fuzzing, TAS search. If you want to *play* a GBA game, use the
CPU reference core in `src/cpu_ref/`, which exists anyway as the correctness
oracle for the GPU core.

## Build

```sh
source env.sh                # required: see docs/device-limits.md for why
./tools/fetch_test_roms.sh   # one-time: test ROMs for the M2+ gates
cmake -S . -B build -G Ninja
cmake --build build
./build/m0_square      # M0 gate: compute shader round-trip
./build/m1_membus      # M1 gate: memory bus behaviour (CPU)
./build/m1_parity      # M1 gate: CPU and GPU builds agree
./tools/run_cpu_tests.sh   # M2/M3 gate: jsmolka CPU test ROMs
./build/m4_gpu_test        # M4 gate: same ROM inside the compute shader
./build/m5_ppu             # M5 gate: bitmap modes, writes mode{3,4,5}.png
./build/m6_sprites         # M6 gate: sprite flips, sizes, bounds
./build/m6_effects         # M6 gate: priority, windows, blending, affine
./build/render_rom <rom> [frames] [out.png]   # render any ROM
./tools/run_gates.sh       # everything at once
```

`./build/m8_bench <rom> [cycles] [render|norender]` measures throughput against
instance count. See `docs/performance.md` for the full results: ~10x one CPU
core at 4096 instances, and two of the plan's performance predictions measured
and refuted.

Test ROMs are not vendored; run `./tools/fetch_test_roms.sh` once to clone
`jsmolka/gba-tests` into `third_party/`.

No BIOS image is needed or shipped. The GBA BIOS is copyrighted, so
`src/core/bios.inc` implements the SWI calls directly (Div, Sqrt, CpuSet,
CpuFastSet, Halt) and `hleBoot` sets up the registers the BIOS would have left.

`env.sh` sets `VK_DRIVER_FILES` and `VK_LAYER_PATH`, and writes a patched copy
of the validation-layer manifest with an absolute library path. All of that is
needed on a Homebrew Vulkan install; without it you get "Found no drivers!" or
`VK_ERROR_LAYER_NOT_PRESENT`. See `docs/device-limits.md` for why, including
the macOS SIP behaviour that makes `DYLD_LIBRARY_PATH` useless inside scripts.

## Design in one paragraph

The emulator core is written **once**, in a restricted C subset that compiles
both as C++20 on the host and as GLSL compute for the GPU, selected by a macro
layer in `src/core/types.h`. That buys a differential test harness: run the
same ROM on both builds in lockstep and the first register mismatch names the
exact broken opcode. Debugging a shader-resident ARM7TDMI without that oracle
is not realistic.

## Running many instances

```sh
python3 tools/make_bench_roms.py            # synthetic ROMs for the harness
./build/m9_harness build/roms/input_echo.gba 4096 20
```

The harness is the throughput interface: a distinct controller input per
instance per frame, a 60x40 grayscale observation read back from every instance
each frame, a probe that gathers one word from the same address in every
instance (a reward signal, without reading back whole regions), and snapshot
and restore of machine state.

At 4096 instances it sustains about **2250 instance-frames/s**, roughly 38x
realtime in aggregate. Every claim it makes is checked rather than displayed:
inputs are read back from inside the emulated machine, instance 0's observation
is compared byte for byte against the CPU reference, and a snapshot followed by
a replay of the same inputs must reproduce the same observation exactly.

## Running a commercial game

```sh
./tools/run_game.sh "/path/to/game.gba" 190 out.png
CPU_ONLY=1 VERBOSE=1 ./tools/run_game.sh "/path/to/game.gba" 400   # fast exploration
```

Pokemon Emerald boots and renders its Game Freak screen, with the GPU
byte-identical to the CPU reference. Getting there needed the whole M7 stack
plus 128 KiB Flash save emulation: Emerald sets its main callback to NULL and
does nothing forever if it cannot identify a save chip.

`cpu_test` is the debugging tool for a ROM that misbehaves. It takes
`BRANCHES=1` (log every branch), `BRANCH_FROM=<n>` (start logging after n
instructions), `WATCH=<addr>` (report every instruction that changes a word),
`TRACEPC=<addr>` (dump registers through a code range) and `HIST=1` (a profile
of which ROM regions the game actually executes).

## What the PPU covers

Verified against test ROMs or explicit checks: text backgrounds (modes 0-2),
bitmap modes 3/4/5, affine backgrounds with and without display-area overflow,
sprites with both flips and correct bounds, object disable, layer priority,
windows including exact edge semantics, alpha blending, brightness increase and
decrease, and affine sprites (an identity matrix reproduces a plain sprite
exactly).

Implemented but **not yet covered by a test**: mosaic, the object window,
semi-transparent sprites, 8bpp background tiles, background maps wider or
taller than 256, two-dimensional sprite tile mapping, and sprite sizes other
than 16x16. These are the first places to look if a real game renders wrongly.

Rendering is scanline granular, so mid-scanline register writes -- raster
effects like a per-line gradient or a wobble -- will not reproduce. That is a
deliberate scope decision from the plan.

## A note on M5's test ROMs

jsmolka's `ppu/` ROMs all set BG0CNT: they are tiled-mode tests and belong to
M6. The suite has no bitmap-mode ROM, so `m5_ppu` hand-assembles three small
ones (in `src/app/m5_ppu.cpp`) that draw index-derived patterns in modes 3, 4
and 5. They are real ARM machine code run by the emulator rather than
host-poked memory, since the milestone is about a ROM drawing something.

The mode 4 ROM writes pixels a halfword at a time. Writing them individually
with `STRB` does not work on hardware — an 8-bit write to VRAM is doubled
across the containing halfword, so each store clobbers its neighbour.

## A note on M2 and M3

The plan treated the ARM and Thumb interpreters as separable milestones. They
are not: `arm.gba` test 50 deliberately `BX`es into Thumb code to verify the
mode transition, so the ARM gate cannot pass without a working Thumb decoder.
They were implemented and gated together.

## Milestones

| | | Status |
|---|---|---|
| M0 | Toolchain + compute shader round-trip | **done** |
| M1 | Core scaffold, memory map, dual-compile proven | **done** |
| M2 | ARM interpreter — passes `arm.gba` | **done** |
| M3 | Thumb interpreter — passes `thumb.gba`, `memory.gba` | **done** |
| M4 | Same core on GPU, verified against CPU | **done** |
| M5 | PPU bitmap modes 3/4/5 — first pixels | **done** |
| M6 | Tiled modes, sprites, windows, blending | **done** |
| M7 | DMA, timers, interrupts, BIOS, Flash saves | **mostly** |
| M8 | Scale out; measure divergence and memory layout | **done** |
| M9 | Throughput harness | **done** |

See `docs/device-limits.md` for measured hardware limits and the instance-count
budget.
