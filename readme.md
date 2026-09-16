# gba-gpu

A Game Boy Advance emulator where the **entire console runs inside a Vulkan
compute shader** — ARM7TDMI CPU, memory bus, PPU, DMA, timers and interrupts —
with one GPU invocation per emulated console, so thousands of independent GBA
instances run at once.

Built on an Apple M4 via MoltenVK. Priority is learning, so the code favours
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
source env.sh          # required: see docs/device-limits.md for why
cmake -S . -B build -G Ninja
cmake --build build
./build/m0_square      # M0 gate: compute shader round-trip
./build/m1_membus      # M1 gate: memory bus behaviour (CPU)
./build/m1_parity      # M1 gate: CPU and GPU builds agree
```

`env.sh` sets `VK_DRIVER_FILES`, `VK_LAYER_PATH` and `DYLD_LIBRARY_PATH`.
All three are needed on a Homebrew Vulkan install; without them you get
"Found no drivers!" or `VK_ERROR_LAYER_NOT_PRESENT`.

## Design in one paragraph

The emulator core is written **once**, in a restricted C subset that compiles
both as C++20 on the host and as GLSL compute for the GPU, selected by a macro
layer in `src/core/types.h`. That buys a differential test harness: run the
same ROM on both builds in lockstep and the first register mismatch names the
exact broken opcode. Debugging a shader-resident ARM7TDMI without that oracle
is not realistic.

## Milestones

| | | Status |
|---|---|---|
| M0 | Toolchain + compute shader round-trip | **done** |
| M1 | Core scaffold, memory map, dual-compile proven | **done** |
| M2 | ARM mode interpreter (CPU) — passes `arm.gba` | |
| M3 | Thumb mode interpreter (CPU) — passes `thumb.gba` | |
| M4 | Same core on GPU, lockstep-verified against CPU | |
| M5 | PPU bitmap modes — first pixels | |
| M6 | Tiled modes and sprites | |
| M7 | DMA, timers, interrupts, input | |
| M8 | Scale out; measure divergence and memory layout | |
| M9 | Throughput harness | |

See `docs/device-limits.md` for measured hardware limits and the instance-count
budget.
