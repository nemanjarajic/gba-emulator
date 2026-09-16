# Running on Windows with an NVIDIA GPU

Verified on an RTX 5060 Ti (8 GB, driver 596.49) under Windows 11, built with
MSVC 19.31 and the LunarG Vulkan SDK 1.4.357. Every gate in
`tools/run_gates.sh` passes, including CPU/GPU byte-identity from M1 through
M9 and the C API test.

## What is portable, and why

- **The shaders need nothing unusual.** `gba.spv` declares exactly one SPIR-V
  capability, `Shader`, the Vulkan 1.0 baseline: no subgroup operations, no
  8- or 16-bit storage, no 64-bit integers. Only the M0 smoke test
  (`square.comp`) asks for more, and only `GroupNonUniform`. Verify with
  `spirv-dis build/shaders/gba.spv | grep OpCapability`.
- **GLSL is compiled on the host, not by the driver.** `glslc` produces SPIR-V
  at build time and NVIDIA consumes that, so GLSL parsing is identical on both
  platforms. There is no "stricter driver" risk in the shader source.
- **`VK_KHR_portability_enumeration` and `VK_KHR_portability_subset` are
  queried, never assumed.** They exist for MoltenVK; their absence is fine.
- **Buffer allocation tries memory types in order** and falls back to
  device-local plus a staging copy, so it does not depend on unified memory or
  on Resizable BAR. `gpu_check` prints which path it landed in.
- **A discrete GPU is preferred** over an integrated one, whatever order the
  driver enumerates them in. `GBA_DEVICE=<index>` picks a device explicitly.
- **`-Wall -Wextra` is only used off MSVC**, which gets `/W3` instead. MSVC
  reports `getenv` as deprecated (C4996); that warning is expected.

## Build

Needs:

- The LunarG Vulkan SDK, for `glslc`, the headers and the loader import
  library. `winget install KhronosGroup.VulkanSDK`.
- **CMake 3.24 or newer.** The CMake bundled with Visual Studio 2022 17.1 is
  3.22 and is refused. `winget install Kitware.CMake`.
- A C++20 compiler. MSVC is what has been used; clang-cl and MinGW are
  untested.

Build from an "x64 Native Tools Command Prompt for VS 2022" (or after running
`vcvars64.bat`), with Ninja so the executables land directly in `build\` where
the gate scripts expect them:

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
```

The Visual Studio generator works too, but puts executables in
`build\RelWithDebInfo\`, which `tools/*.sh` do not look in.

`env.sh` is macOS-only and is not needed: on Windows the loader finds the
driver and the validation layers through the registry. The `tools/*.sh`
scripts run under Git Bash; the executables themselves do not need it.

## Run this first

```
build\gpu_check.exe third_party\gba-tests\arm\arm.gba 4096
```

It prints the device, the limits that matter, how many instances fit in each
configuration, and re-measures the two results that came out
counter-intuitively on Apple Silicon.

On the RTX 5060 Ti:

| | |
|---|---|
| baseline, 4096 instances | **8872 Mcycle/s** |
| buffer memory | host-mapped device-local (Resizable BAR on) |
| subgroup size | 32 |
| `GbaState` + 4 words | 1836 Mcycle/s, **0.21x** |
| interleaved memory layout | 3805 Mcycle/s, **0.43x** |

Both Apple Silicon findings hold here, and more strongly:

- **The register cliff is real on NVIDIA too.** Four extra words in `GbaState`
  cost nearly 5x, against 2.9x on an M4. The size assertion in
  `src/core/state.h` stays.
- **The interleaved layout loses** by more than half. Keep the default.

## The first run is slow; later runs are not

The first time each distinct pipeline is created, the NVIDIA driver spends
about **two minutes** compiling it, on the CPU, with the GPU idle. The full
gate suite took 19 minutes cold, and `gpu_check` 6 minutes, almost all of it
this.

The driver caches the result on disk (`%LOCALAPPDATA%\NVIDIA\GLCache`), so a
second run of the same gate takes a second or two. Nothing in this repository
needs to change for that; an application `VkPipelineCache` would duplicate the
driver's. But any edit under `src/core/` changes the SPIR-V and pays the
compile again, so expect it after every core change.

## Instance counts on 8 GB

As reported by `gpu_check`:

| configuration | per instance | fits |
|---|---|---|
| headless, with save | 525 KB | 13,089 |
| headless, no save | 397 KB | 16,383 (buffer limit) |
| observations + save | 600 KB | 11,452 |
| observations, no save | 472 KB | 14,560 |

Two limits bind before memory does:

- **`maxStorageBufferRange`** is 4 GiB. EWRAM is 256 KiB per instance, so it
  reaches that at 16,384 instances. The pool refuses to allocate past it rather
  than failing inside Vulkan.
- **Occupancy.** See the throughput section below: raw emulation peaks at
  9216 instances and a full harness frame at 4096, both well inside memory.

### Reclaiming the save memory

Save memory is 128 KiB per instance, a quarter of the footprint, and a rollout
that never saves does not need it:

```cpp
pool.create(ctx, instances, romWords, withFramebuffers, /*withSave=*/false);
CorePush push{instances, romWords, cycles, pool.baseFlags() | FLAG_RENDER};
```

`baseFlags()` supplies `FLAG_NO_SAVE` automatically; passing plain `0` for the
flags with a pool built this way would index a buffer that is too small.

**But note:** some games refuse to run without save memory at all. Pokemon
Emerald identifies its flash chip on boot and, finding none, sets its main
callback to NULL and draws nothing forever. Check the game boots before
committing to this.

## Throughput

### Raw emulation: 8.8x the M4

`m8_bench third_party/gba-tests/arm/arm.gba`, 2,000,000 cycles per instance,
rendering off. CPU baseline 94 Mcycle/s on one core.

| instances | agg Mcycle/s | MHz/instance | x one CPU core | M4 |
|---|---|---|---|---|
| 1 | 2.4 | 2.38 | 0.03 | 0.4 |
| 256 | 607 | 2.37 | 6 | 99 |
| 1024 | 2,418 | 2.36 | 26 | 395 |
| 4096 | 9,481 | 2.31 | 100 | 1,074 |
| 8192 | 16,475 | 2.01 | 175 | 974 |
| **9216** | **18,278** | 1.98 | 194 | |
| 9280 | 10,415 | 1.12 | 110 | |
| 12288 | 13,099 | 1.07 | 139 | |

A single instance is six times the M4's, and scaling is linear to 8192.

**There is a cliff at exactly 9216 instances.** At 9280 the per-instance clock
halves and aggregate throughput falls 43%; beyond that it climbs linearly
again at the lower rate, and never recovers the 9216 figure before memory runs
out. It is not memory: rendering on adds 75 KB per instance and the cliff stays
between 9216 and 9280. It is not dispatch length either: `DISPATCH=16384`
and `DISPATCH=262144` land on it identically. 9216 is 144 work groups of 64,
and twice the card's 4608 CUDA cores; which of those, if either, is the cause
is not established.

Two measurement notes. Polling `nvidia-smi` while benchmarking depressed the
results by up to 3x, so do not monitor that way. And first runs of any new
pipeline include the compile described above; `m8_bench` excludes it from
its timings, but its wall-clock time does not.

### The throughput harness: readback was the bottleneck

`m9_harness build/roms/input_echo.gba <n> 60`, rendering and observations on,
reading every instance's observation and a probe back every frame:

| instances | before | after | M4 |
|---|---|---|---|
| 1024 | 2,445 | 4,227 | 996 |
| 4096 | 4,050 | **15,253** | 2,253 |
| 6144 | | 12,938 | |
| 8192 | 3,637 | 10,500 | |
| 9216 | 3,558 | 10,045 | |

(instance-frames/s)

Before, observation readback was 74% of the harness's wall time at 4096
instances: 0.74 s a frame to copy 9.4 MiB. Resizable BAR maps the pool into
the host's address space, and `downloadBuffer` memcpy'd straight out of that
window, which on unified memory is free and over PCIe is not.
`downloadBuffer` now has the GPU copy anything larger than 64 KiB into a
host-cached staging buffer and memcpys from there, keeping the direct read on
unified memory and for small reads. Readback at 4096 went from 44.6 s to 0.19 s
over the 60 frames.

With a frame's worth of rendering in every dispatch, the useful maximum is
**4096**, not 9216: 6144 already loses.

Through the Python bindings (`gba-emulator-rl/examples/benchmark.py`, same
ROM, action repeat 4), 4096 instances went from 2,385 to **4,145 agent
steps/s**. The gain is smaller than the harness's because only the last of the
four frames is observed.

### Pokemon Emerald

A real game, not a synthetic ROM. `tools/run_game.sh` boots it to the Game Freak
screen with zero register mismatches and a byte-identical framebuffer against
the CPU reference, as on the M4.

`m8_bench`, 2,000,000 cycles per instance from boot, rendering off. CPU
baseline 51 Mcycle/s on one core.

| instances | agg Mcycle/s | MHz/instance | M4 |
|---|---|---|---|
| 1024 | 870 | 0.85 | 319 |
| 4096 | 3,442 | 0.84 | 812 |
| 8192 | 5,004 | 0.61 | 792 |
| 9216 | 5,576 | 0.61 | |

Emerald is about a third of `arm.gba`'s throughput here, against three quarters
on the M4, so the card's lead over the M4 narrows to 4.2x at 4096 and 6.3x at
8192. Per-instance speed starts to fall at 8192 rather than 9216.

Through the Python bindings, action repeat 4, which is the number a training
run sees:

| instances | agent steps/s | x realtime | M4 |
|---|---|---|---|
| 256 | 105 | 7 | 43 |
| 1024 | 407 | 27 | 168 |
| 4096 | 1,529 | 102 | 397 |
| 8192 | 2,463 | 165 | |
| **9216** | **2,737** | 183 | |
| 9280 | 1,370 | 92 | |

Unlike the synthetic harness, Emerald keeps gaining up to 9216, and the cliff
is at exactly the same place. **9216 is the instance count to use for Emerald**:
about 9.9M agent steps an hour, against 1.4M on the M4.

Creating the environment takes about 20 seconds at that size, and the first
4096 cycles take another 16-18: Emerald's boot clears all of RAM through
`RegisterRamReset`, which is the first touch of several GB of freshly
allocated GPU memory. It is a one-off per process.

**Above 9720 instances, Emerald with rendering loses the device.** 9720 works
and 9721 fails with `VK_ERROR_DEVICE_LOST` on the first dispatch, logged by the
driver as `nvlddmkm` event 153. What is known:

- It needs all three of Emerald, rendering, and more than 9720 instances.
  Emerald with rendering off runs at 12,288; `arm.gba` with rendering runs at
  10,240 with the same buffers.
- It is not dispatch length. The failing dispatch is 4096 cycles, the same one
  that takes 18 s and succeeds at 9720; splitting frames into dispatches as
  short as 2048 cycles changes nothing.
- It is not total memory: `arm.gba` at 10,240 allocates more.
- It is not an out-of-range VRAM read in the PPU. The CPU core, instrumented,
  sees none in 1500 frames.

Since everything above 9216 is slower anyway, it costs nothing in practice, but
it is a crash rather than a slowdown. Reproduce with
`ONLY=9721 ./build/m8_bench <emerald.gba> 8192 render`.

## Known gaps

- `InstancePool::readProbe` falls back to one small transfer per instance when
  buffers are not mapped, which will be slow for a per-frame reward signal on
  a non-ReBAR system. Worth replacing with a single strided download if that
  path turns out to matter. (With Resizable BAR on, probes gather straight out
  of the mapping; being a few scattered words, that is not slow.)
- Resizable BAR off is still untested. Large reads now use the staging path
  regardless, so that half is exercised; uploads and small reads through
  staging are not.
- The 9216-instance cliff is measured, not explained.
- Emerald with rendering crashes above 9720 instances; see above.
