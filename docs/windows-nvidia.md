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
- **Occupancy.** On an M4 throughput saturated at 4096 instances, well below
  what memory allowed. The useful maximum here has not been swept yet; do it
  with `m8_bench` rather than assuming.

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

- The staging path in `uploadBuffer`/`downloadBuffer` has still never
  executed: with Resizable BAR on, every buffer is host-mapped here as on Apple
  Silicon. It would run only on a discrete card with Resizable BAR off.
- `InstancePool::readProbe` falls back to one small transfer per instance when
  buffers are not mapped, which will be slow for a per-frame reward signal on
  a non-ReBAR system. Worth replacing with a single strided download if that
  path turns out to matter.
- The throughput sweep (`m8_bench`, `m9_harness` at scale) has not been run on
  this card, so `docs/performance.md` still describes Apple Silicon only.
