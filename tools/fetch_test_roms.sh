#!/bin/sh
# Fetches the jsmolka CPU/PPU test ROMs used by the M2+ gates.
# Not vendored: it is a separate upstream repository with its own history.
set -e
cd "$(dirname "$0")/.."
if [ -d third_party/gba-tests ]; then
    echo "third_party/gba-tests already present"
    exit 0
fi
git clone --depth 1 https://github.com/jsmolka/gba-tests third_party/gba-tests
