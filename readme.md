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
./build/m6_coverage        # M6 gate: 8bpp, big maps, sprite sizes, mosaic, obj window
./build/m7_io              # M7 gate: keypad IRQ, FIFO DMA, SoftReset, IntrWait
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

## Embedding

`src/api/gba_env.h` is a C ABI that presents the emulator as a vectorised
environment: per-instance actions, downsampled observations, RAM probes for
reward signals, and per-instance episode reset. `libgba_env` is what an
embedder links against, and `cmake --install` exports it with its header and
the compiled shaders.

The interface is versioned. `gba_env_abi_version()` lets a caller refuse a
library that does not match what it was built against, rather than crashing or
reading wrong data. `build/api_test` exercises the whole ABI, so a change to
the header is caught here rather than by whoever loads the library next.

The Python bindings that use it are a separate repository, `gba-rl`, which
finds this one through `GBA_ENV_LIB`, `GBA_EMULATOR_ROOT`, or simply by sitting
beside it.

## Running many instances

```sh
python3 tools/make_bench_roms.py            # synthetic ROMs for the harness
./build/m9_harness build/roms/input_echo.gba 4096 20
```

The harness is the throughput interface: a distinct controller input per
instance per frame, a 120x80 grayscale observation read back from every instance
each frame, a probe that gathers one word from the same address in every
instance (a reward signal, without reading back whole regions), and snapshot
and restore of machine state.

At 4096 instances it sustains about **2250 instance-frames/s**, roughly 38x
realtime in aggregate. Every claim it makes is checked rather than displayed:
inputs are read back from inside the emulated machine, instance 0's observation
is compared byte for byte against the CPU reference, and a snapshot followed by
a replay of the same inputs must reproduce the same observation exactly.

## Playing a game

```sh
brew install sdl3            # optional; the debugger is only built if present
./build/debugger "<rom>.gba"
```

An SDL window with the screen at 3x, and beside it the register file, live
disassembly around the PC, a memory view and breakpoints. Space runs and
pauses, `n` steps an instruction, `f` steps a frame, `b` toggles a breakpoint at
the PC, `m` cycles the memory view, F5 and F9 save and restore state. Buttons
are Z and X, enter and right-shift, the arrow keys, and Q and W for the
shoulders.

`--png out.png --frames N` renders the whole interface to a file without
opening a window, which is how its layout is checked.

**The debugger runs on the CPU core, not the GPU, and that is not a
compromise.** One instance on the GPU manages 0.33 MHz against the 16.78 MHz a
GBA needs; the CPU core does 96 Mcycle/s, about six times real time. The GPU
path exists to run thousands of machines at once and cannot run one of them at
playable speed.

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

## Other GPUs

`./build/gpu_check <rom> <instances>` is the preflight: it reports the device,
the limits that matter, how many instances fit, and re-measures the two results
that came out counter-intuitively on Apple Silicon (the register cliff and the
memory layout) so you learn which way they go on yours. See
`docs/windows-nvidia.md` for a Windows/NVIDIA bring-up guide.

## A trap worth knowing about

`GbaState` is 83 words, and that is the largest it can be before the shader's
register allocator spills and throughput drops by about 3x. Adding a single
field to it costs two thirds of the emulator's speed. The static assertion in
`src/core/state.h` fails if it grows; new per-instance state belongs in its own
storage buffer. See `docs/performance.md` for the measurements.

## What the hardware covers

Implemented and gated: the ARM7TDMI in both instruction sets, the full memory
map, all four DMA channels (immediate, VBlank, HBlank and the audio FIFO
trigger), the four timers with prescalers and cascade, the interrupt controller,
keypad interrupts, 128 KiB Flash saves, and a BIOS covering the arithmetic
helpers, the decompressors, the affine matrix helpers, SoftReset and IntrWait.

**Not implemented:** audio output (the FIFO DMA drains so a sound driver does
not stall, but nothing is mixed or played), the serial port, the cartridge RTC
that Pokemon Emerald uses for time-based events, and the BIOS sound-driver,
multiplayer and diff-filter calls. An unimplemented SWI takes a real exception,
which the installed vector table turns into a return rather than a crash.

## What the PPU covers

Verified against test ROMs or explicit checks: text backgrounds (modes 0-2),
bitmap modes 3/4/5, affine backgrounds with and without display-area overflow,
sprites with both flips and correct bounds, object disable, layer priority,
windows including exact edge semantics, alpha blending, brightness increase and
decrease, and affine sprites (an identity matrix reproduces a plain sprite
exactly).

Also verified (`m6_coverage`): 8bpp background tiles, background maps of every
screen size with scrolling between screenblocks, one- and two-dimensional
sprite tile mapping, all twelve sprite shape/size combinations, mosaic, the
object window, and semi-transparent sprites. Every one of these passed first
time, which is worth knowing: they were written from the documentation and then
left unexercised until now.

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
| M7 | DMA, timers, interrupts, BIOS, Flash saves | **done** |
| M8 | Scale out; measure divergence and memory layout | **done** |
| M9 | Throughput harness | **done** |

See `docs/device-limits.md` for measured hardware limits and the instance-count
budget.
