# Running on Windows with an NVIDIA GPU

Everything here is written for that target but **has never been run on it**.
The portability work is real and reasoned, not verified. This document says
what to expect, what to check first, and where the risks are.

## What is already portable

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
- **`-Wall -Wextra` is only used off MSVC**, which gets `/W3` instead.

## Build

Needs the LunarG Vulkan SDK (for `glslc` and the loader), CMake and a C++20
compiler. MSVC, clang-cl and MinGW should all work; only AppleClang has been
used so far.

```
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build --config RelWithDebInfo
```

`env.sh` is macOS-only and is not needed: on Windows the loader finds the
driver and the validation layers through the registry. The `tools/*.sh` scripts
need Git Bash or WSL; the executables themselves do not.

## Run this first

```
build\gpu_check.exe third_party\gba-tests\arm\arm.gba 4096
```

It prints the device, the limits that matter, how many instances fit in each
configuration, and then measures the two results that came out
counter-intuitively on Apple Silicon, telling you which way they go on yours:

- **The register cliff.** `GbaState` is 83 words, and on an M4 adding four more
  costs 2.9x throughput because the register allocator spills. NVIDIA has a
  much larger register file and allows up to 255 registers per thread, so this
  ceiling will sit somewhere different. If `gpu_check` reports no cliff, the
  struct has room to grow and the note in `src/core/state.h` can be relaxed.
- **The memory layout.** The interleaved (structure-of-arrays) layout lost by
  21-45% on an M4. A discrete GPU's cache hierarchy is different enough that it
  may win. If it does, build with `GBA_SOA` and re-run the gates.

Then run the gates: `tools/run_gates.sh` under Git Bash, or the executables
individually.

## Instance counts on 8 GB

Per instance, measured rather than estimated:

| configuration | per instance | fits in ~7 GB |
|---|---|---|
| headless, with save | 518 KB | ~14,200 |
| headless, no save | 390 KB | ~18,800 |
| observations + save | 593 KB | ~12,400 |
| observations, no save | 465 KB | ~15,800 |

Two limits bind before those numbers, and `gpu_check` reports both:

- **`maxStorageBufferRange`.** EWRAM is 256 KiB per instance, so it reaches
  4 GiB -- a typical cap -- at 16,384 instances. The pool refuses to allocate
  past it rather than failing inside Vulkan.
- **Occupancy.** On an M4 throughput saturated at 4096 instances, well below
  what memory allowed. Expect the useful maximum on a 5060 to be somewhere
  around 8,000-12,000 and **sweep it** with `m8_bench` rather than assuming.

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

## Known gaps

- Never compiled or run on Windows or NVIDIA. Treat the first run as a
  bring-up, not a regression test.
- The staging path in `uploadBuffer`/`downloadBuffer` has never executed: on
  Apple Silicon every buffer is host-mapped, so the fallback is untested code.
  It will be exercised on a discrete card only if Resizable BAR is off.
- `InstancePool::readProbe` falls back to one small transfer per instance when
  buffers are not mapped, which will be slow for a per-frame reward signal on
  a non-ReBAR system. Worth replacing with a single strided download if that
  path turns out to matter.
