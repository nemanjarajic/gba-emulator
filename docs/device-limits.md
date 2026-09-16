# Measured device limits (Apple M4, MoltenVK 1.4.2)

Produced by `./build/m0_square`. Re-run it after any driver update rather than
trusting these numbers.

| Limit | Value | Why it matters |
|---|---|---|
| `subgroupSize` | **32** | 32 GBA instances share one SIMD group and serialise on divergent branches. This is *the* performance constant of the project. |
| `maxStorageBufferRange` | 4.00 GiB | Per-buffer cap. At 8192 instances the largest region (EWRAM) is 2 GiB, so we fit with room to spare. |
| `maxMemoryAllocationSize` | 8 GiB | Per-allocation cap. |
| device-local heap | 16 GiB (budget ~10.7 GiB) | Caps instance count. See below. |
| `maxPerStageDescriptorStorageBuffers` | 31 | We need ~10 (one per memory region). Comfortable. |
| `maxComputeWorkGroupInvocations` | 1024 | Workgroup size ceiling. |
| `maxComputeSharedMemorySize` | 32 KiB | Too small to hold any GBA region; shared memory is not useful to us. |

Extensions confirmed present: `VK_KHR_portability_subset`,
`VK_KHR_shader_non_semantic_info`, `VK_KHR_8bit_storage`,
`VK_KHR_16bit_storage`, `VK_EXT_subgroup_size_control`.

## Instance count budget

Per-instance state is ~515 KiB (EWRAM 256K + IWRAM 32K + VRAM 96K + PRAM 1K +
OAM 1K + I/O 1K + save 128K). ROM and BIOS are shared and excluded.

| Instances | State | Verdict |
|---|---|---|
| 1024 | 0.50 GiB | comfortable |
| 4096 | 2.01 GiB | comfortable |
| 8192 | 4.02 GiB | fits inside the ~10.7 GiB budget |
| 16384 | 8.05 GiB | too close to the budget; also past useful GPU occupancy |

## Corrections to the original plan

- The plan justified splitting state into one buffer per memory region by
  claiming MoltenVK reports a `maxStorageBufferRange` "well below" what a
  single packed buffer would need. **That was wrong** -- it reports 4 GiB.
  The split is still the right choice, but the reason is solely that it makes
  the M8 AoS -> interleaved-SoA experiment a one-file change.
- `VK_KHR_8bit_storage` and `VK_KHR_16bit_storage` are both available, so the
  "all RAM is `uint[]` with shift-and-mask" rule is a portability and
  C++-parity choice, not a hardware constraint. It stays, but a byte-addressed
  fallback exists if M8 measurements favour it.

## Toolchain gotchas (both cost real time)

1. Homebrew puts the MoltenVK ICD manifest in `etc/vulkan/icd.d/`, not the
   `share/vulkan/icd.d/` the loader searches. Without `VK_DRIVER_FILES` the
   loader reports "Found no drivers!" despite a correct install.
2. The Homebrew validation-layer manifest names its library relatively, so
   dyld cannot find it. `vkCreateInstance` then fails with
   `VK_ERROR_LAYER_NOT_PRESENT` even though the loader successfully locates and
   parses the manifest.

3. The obvious fix for (2), `DYLD_LIBRARY_PATH=/opt/homebrew/lib`, **does not
   survive a shell script.** macOS System Integrity Protection strips every
   `DYLD_*` variable when it launches a protected binary, and `/bin/sh` is
   protected. The symptom is confusing: every Vulkan program works when run
   straight from the terminal and fails from inside a wrapper script, with no
   message explaining why. `tools/run_gates.sh` hit exactly this.

All three are handled by `source env.sh`, which sidesteps (2) and (3) together
by writing a patched copy of the layer manifest under `build/vulkan/` with an
absolute `library_path`, so no `DYLD_*` variable is needed at all. Verify with
`env -u DYLD_LIBRARY_PATH ./build/m0_square`.

## Cross-platform notes (macOS + Windows/NVIDIA)

The project targets both an Apple Silicon Mac via MoltenVK and a Windows machine
with a discrete NVIDIA GPU. Two places in the Vulkan layer carry that:

- **`createStorageBuffer` tries memory types in preference order** rather than
  choosing by property flags alone, and falls back on allocation failure:
  1. `DEVICE_LOCAL | HOST_VISIBLE` — full GPU bandwidth plus a mapped pointer.
     Free on Apple's unified memory; on a discrete card this is the Resizable
     BAR window.
  2. `DEVICE_LOCAL` alone — host access goes through a staging copy. This is
     where a discrete card lands **without** Resizable BAR, whose host-visible
     window is only 256 MB, far below what a few thousand instances need.
  3. `HOST_VISIBLE` alone — last resort.

  The distinction that matters between tiers 1 and 2 is whether the allocation
  actually *fits*, which the property flags do not express, so the tiers are
  attempted rather than merely inspected. Call sites use
  `uploadBuffer`/`downloadBuffer`/`fillBuffer` and are identical on both paths;
  `m4_gpu_test` prints which one is in use.

- **`VK_KHR_portability_enumeration` is queried before being enabled.** MoltenVK
  needs it and the matching instance-create flag, or `vkEnumeratePhysicalDevices`
  returns nothing on macOS. It is loader-provided, so a current Windows loader
  offers it too, but an older one does not and requesting it unconditionally
  would fail instance creation.

Nothing in `src/core/` is platform-dependent, and NVIDIA's warp size is 32,
matching Apple's SIMD group width, so the divergence model carries over intact.

Still macOS-only: `-Wall -Wextra` in CMake (fine for clang-cl/MinGW, not MSVC)
and the `tools/*.sh` scripts (need Git Bash or WSL).
