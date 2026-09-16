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
exit $fail
