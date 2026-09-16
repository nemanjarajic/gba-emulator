#!/bin/sh
# Renders a commercial ROM and compares the CPU and GPU cores.
#   tools/run_game.sh <rom> [frames] [out.png]
# Set CPU_ONLY=1 to skip the GPU (much faster when exploring), VERBOSE=1 for a
# per-frame report of DISPCNT, the interrupt state and the PC.
set -e
cd "$(dirname "$0")/.."
exec ./build/render_rom "$1" "${2:-190}" "${3:-frame.png}"
