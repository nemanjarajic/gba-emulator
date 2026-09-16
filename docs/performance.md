# Measured throughput (M4)

Apple M4, 10 GPU cores, MoltenVK. Running `arm.gba` to completion (203,196
cycles) on every instance simultaneously. **No M8 optimisation yet**: still the
array-of-structures memory layout, so nothing coalesces.

Reproduce with `./build/m4_gpu_test third_party/gba-tests/arm/arm.gba <N>`.

| instances | Mcycle/s | MHz/instance | × realtime | × one CPU core |
|---|---|---|---|---|
| 1 | 1.1 | 1.09 | 0.07 | 0.01 |
| 64 | 70.1 | 1.09 | 4 | 0.5 |
| 256 | 256.6 | 1.00 | 15 | 1.4 |
| 1024 | 1110.5 | 1.08 | 66 | 6.7 |
| 2048 | 2183.7 | 1.07 | 130 | 11.8 |
| 4096 | 3931.8 | 0.96 | 234 | 20.9 |
| 8192 | 4037.6 | 0.49 | 241 | 21.7 |

CPU baseline: **146 Mcycle/s on one core**, about 9× realtime.

## What this says

- **Scaling is essentially linear to 4096 instances.** Per-instance clock holds
  at roughly 1.0-1.09 MHz the whole way, so nothing is contended yet.
- **8192 instances is past the cliff.** Aggregate throughput barely moves
  (3932 → 4038) while per-instance clock halves. That is the occupancy ceiling
  of a 10-core M4: the extra instances are queued, not running. 4096 is the
  useful maximum on this machine, and it is a scheduling limit, not the memory
  limit — 8192 instances still fit comfortably in the 10.7 GiB budget.
- **Break-even against one CPU core is around 200 instances.** Below that the
  GPU is simply slower, which is the expected shape for this design.
- **The best real number is ~21× one CPU core** at 4096 instances.

## Against the plan's predictions

The plan committed to specific numbers before any code existed. Scoring them:

| Prediction | Actual | |
|---|---|---|
| 0.5-2 MHz per instance | 0.96-1.09 MHz | correct |
| 2-8 Gcycle/s aggregate at 4096 | 3.9 Gcycle/s | correct |
| 120-480× realtime | 234× | correct |
| 10-40× one CPU core | 21× | correct |
| One CPU core is 10-20× realtime | 9× | **too optimistic** |
| Single instance 20-50× slower than a CPU | ~135× slower | **badly underestimated** |

The two misses are the same miss: the C++ reference interpreter is much faster
than assumed (146 Mcycle/s, ~9× realtime), so the per-instance gap to the GPU is
far wider than predicted. The aggregate conclusions were unaffected, because
they were derived from the GPU side, which was estimated correctly.

The practical lesson stands and is now measured rather than asserted: **never
run a single instance on the GPU.** Use the C++ core for anything interactive.

## What M8 should attack

In the order the plan already sets out:

1. **Interleaved SoA addressing** in `MEM_IDX`. Right now 32 lanes of a SIMD
   group touch addresses 256 KiB apart, so no two share a cache line. This is
   untested and expected to be the largest single gain.
2. **Divergence measurement.** Every instance here runs the same ROM from the
   same state with no input, so they stay in near-perfect lockstep — this table
   is close to a best case. Real workloads with differing input sequences will
   diverge over a rollout, and the cost of that needs measuring before any
   claim about RL throughput is credible.
3. **Cycles per dispatch.** 16384 is used here with 13 dispatches; the tradeoff
   against the macOS GPU watchdog has not been swept.
