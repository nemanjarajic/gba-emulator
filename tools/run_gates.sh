#!/bin/sh
# Runs every milestone gate. Requires `source env.sh` and a built ./build.
set -e
cd "$(dirname "$0")/.."
fail=0
run() {
    printf '%-12s ' "$1"
    shift
    if out=$("$@" 2>/dev/null); then
        echo "$out" | grep -E 'PASS' | tail -1
    else
        echo "FAILED"
        echo "$out" | tail -20 | sed 's/^/    /'
        fail=1
    fi
}
run M0 ./build/m0_square
run M1-bus ./build/m1_membus
run M1-parity ./build/m1_parity
printf '%-12s\n' "M2/M3"
./tools/run_cpu_tests.sh | sed 's/^/    /' || fail=1
run M4 ./build/m4_gpu_test
run M5 ./build/m5_ppu
run M7-io ./build/m7_io
run M6-sprites ./build/m6_sprites
run M6-effects ./build/m6_effects
printf '%-12s\n' "M6-roms"
for r in stripes shades hello; do
    printf '    %-10s ' "$r"
    if ./build/render_rom "third_party/gba-tests/ppu/$r.gba" 4 "ppu_$r.png" >/dev/null 2>&1; then
        echo "rendered, CPU/GPU identical"
    else
        echo "FAILED"; fail=1
    fi
done
# The M9 harness needs its synthetic ROM.
python3 tools/make_bench_roms.py >/dev/null
run M9 ./build/m9_harness build/roms/input_echo.gba 1024 12

exit $fail
