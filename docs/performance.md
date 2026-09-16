# Measured throughput

Apple M4, 10 GPU cores, MoltenVK. Reproduce with `./build/m8_bench <rom>
[cycles] [render|norender]`, plus the `ONLY`, `DISPATCH`, `DIVERGE` and
`SHADER` environment variables described below.

## Scaling with instance count

`arm.gba`, 2,000,000 cycles per instance, rendering off, array-of-structures
layout. CPU baseline **105 Mcycle/s on one core** (6.3x realtime).

| instances | agg Mcycle/s | MHz/instance | × realtime | × one CPU core |
|---|---|---|---|---|
| 1 | 0.4 | 0.39 | 0.02 | 0.004 |
| 32 | 12.4 | 0.39 | 1 | 0.1 |
| 256 | 98.7 | 0.39 | 6 | 0.9 |
| 1024 | 394.6 | 0.39 | 24 | 3.8 |
| 2048 | 729.8 | 0.36 | 43 | 6.9 |
| 4096 | **1074.1** | 0.26 | 64 | **10.2** |
| 8192 | 973.9 | 0.12 | 58 | 9.3 |

Pokemon Emerald, same conditions: 319 Mcycle/s at 1024, **812 at 4096**, 792 at
8192 — about 25% below the synthetic ROM, which is the honest figure to quote
for a real workload.

Scaling is linear to ~2048 and saturates at 4096. Past that the extra instances
are queued rather than run: 8192 is slower in aggregate *and* halves the
per-instance clock. **4096 is the useful maximum on this machine**, a
scheduling limit rather than a memory one — 8192 instances still fit the
budget comfortably.

Break-even against a single CPU core is around 300 instances.

## The interleaved layout is slower, not faster

The plan predicted that switching `MEM_IDX` from contiguous-per-instance
(array-of-structures) to interleaved (structure-of-arrays) would be "the single
largest performance change in the project", because 32 lanes of a SIMD group
touching addresses 256 KiB apart can never share a cache line.

**Measured, it is 21-45% slower.** Both layouts are built (`gba.spv` and
`gba_soa.spv`) so this can be re-checked: `SHADER=gba_soa ./build/m8_bench ...`.

| instances | AoS | SoA | |
|---|---|---|---|
| arm.gba 4096 | 1074 | 595 | −45% |
| arm.gba 8192 | 974 | 673 | −31% |
| Emerald 4096 | 812 | 644 | −21% |
| Emerald 8192 | 792 | 647 | −18% |

This is not a case of the benefit failing to appear. Every instance here runs
the same ROM from the same state with no input, so the lanes are in *perfect*
lockstep and all access the same word index — exactly the condition under which
interleaving should coalesce best. It still loses, which makes the result
stronger rather than weaker.

Two plausible reasons, in order of confidence:

1. **Address arithmetic.** The contiguous form is `inst * words + w`, whose
   first term is loop-invariant and gets hoisted once per dispatch. The
   interleaved form is `w * g_num_instances + inst`, a multiply by a runtime
   value on *every* access.
2. **Locality.** Apple Silicon's unified memory and large caches suit the
   contiguous pattern, where an instance's small working set stays resident.
   Interleaving scatters each instance's bytes across the whole buffer.

The contiguous layout stays the default. The macro remains the single point of
change, so this is easy to revisit on a discrete GPU, where the cache hierarchy
is different enough that the answer may well flip.

## Divergence costs nothing until the *decoder* diverges

The plan assumed divergence would be the dominant cost. It depends entirely on
what diverges. Both experiments use a ROM whose instances are steered down 32 equal-cost paths
by a per-instance seed (`DIVERGE=<paths>`), at 4096 instances. Generate the
ROMs with `python3 tools/make_bench_roms.py`.

**Different data, same instruction types** — `diverge.gba`, all paths built
from the same opcodes:

| distinct paths | Mcycle/s | |
|---|---|---|
| 1 | 862 | 100% |
| 4 | 868 | 101% |
| 16 | 873 | 101% |
| 32 | 876 | 102% |

Free. Within noise of lockstep even when all 32 lanes of a SIMD group take
different paths.

**Different instruction classes** — `diverge2.gba`, where each path uses a
different kind of ARM instruction (data-processing, multiply, load, store,
halfword load, block transfer, shifted register), forcing the interpreter's
decode switch onto different branches:

| distinct classes | Mcycle/s | |
|---|---|---|
| 1 | 873 | 100% |
| 2 | 727 | 83% |
| 4 | 537 | 62% |
| 8 | 478 | **55%** |

A repeat run of the endpoints gave 873 and 425, so the 8-class figure sits
around 49-55% depending on the run. The shape is not in doubt; the exact number
is worth about two significant figures.

So the cost is not lanes holding different *data*, it is lanes executing
different *emulator code*. That has a direct consequence for the intended use:
instances running the same game with different inputs stay aligned on
instruction type most of the time, which is the cheap case. It also means the
Emerald numbers above already include realistic decoder divergence, since a
real instruction stream mixes classes constantly.

Note the curve flattens rather than collapsing: 8 classes costs 45%, not the
87% that full serialisation would imply.

## Cycles per dispatch is not a tuning lever

4096 instances, `arm.gba`:

| cycles/dispatch | dispatches | Mcycle/s |
|---|---|---|
| 4,096 | 488 | 1049 |
| 16,384 | 122 | 1070 |
| 65,536 | 30 | 1081 |
| 262,144 | 7 | 1084 |
| 1,048,576 | 2 | 1079 |
| 4,194,304 | 1 | 1074 |

Flat across three orders of magnitude, so per-dispatch overhead is negligible
even at 488 dispatches. Nothing approached the macOS GPU watchdog, including a
single dispatch covering the whole run. 262,144 is kept as the default for
headroom, not for speed.

**One caveat:** runs occasionally stall, taking ~130x longer (8 Mcycle/s
instead of 1080) with no change in configuration. It reproduced once at 65,536
cycles/dispatch and then did not on a retry of the identical command, so it is
intermittent and *not* a property of the dispatch size. The leading hypothesis
is host memory pressure — a 4096-instance pool is ~2.1 GB and the benchmark
allocates and frees one per configuration. Worth re-checking before trusting
any single measurement.

## Hot-path work that did pay off

M7 cost about 3x, because the scheduler now runs after every instruction.
Two fixes recovered a large part of it:

- **Timer early-out.** `timer_tick` read four control registers per instruction
  just to discover nothing was enabled. A cached `timer_active` bitmask makes
  the common case free, and the live counter now lives in the state struct with
  the bus read substituting it, instead of being written back to I/O memory
  every cycle.
- **Cached interrupt flag.** `irq_check` runs between every pair of
  instructions and was loading IE/IF from I/O — one uncoalesced access per
  instruction. `irq_ready` is now maintained wherever IE or IF changes.

Together: CPU 77.6 → 105.2 Mcycle/s (+35%), GPU at 4096 862 → 1074 (+25%).

## Two hypotheses that were wrong

Recorded because the measurements cost real time and the conclusions are not
obvious:

- **The sprite scanline buffer is not the problem.** `ppu_render_scanline`
  holds a 240-word `obj_line` array, ~960 bytes of private memory per thread,
  which looked like an obvious occupancy killer. Compiling it out entirely
  changed throughput by 2% (1059 vs 1079).
- **Loop unrolling is not what makes the kernel huge.** The kernel is 2.15 MB
  of SPIR-V stripped of debug info, against 2.7 KB for a trivial shader, and
  `-O` unrolling the 240-pixel and 128-sprite loops seemed the likely cause.
  Annotating every large loop with `[[dont_unroll]]` changed the size by zero
  bytes. The size comes from the ARM and Thumb decoders themselves being
  inlined into one enormous function. Whether that exceeds the instruction
  cache is still untested, and remains the most likely explanation for the gap
  between these numbers and the M4-era 1.09 MHz per instance.
