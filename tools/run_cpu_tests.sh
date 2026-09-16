#!/bin/sh
# M2/M3 gate: every CPU test ROM must report a clean pass.
set -e
cd "$(dirname "$0")/.."
status=0
for rom in arm/arm thumb/thumb memory/memory; do
    printf '%-24s ' "$rom"
    if out=$(./build/cpu_test "third_party/gba-tests/$rom.gba" 2>&1); then
        echo "$out" | tail -2 | head -1
    else
        echo "FAILED"
        echo "$out" | sed 's/^/    /'
        status=1
    fi
done
exit $status
