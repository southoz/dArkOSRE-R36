#!/bin/sh
# Regenerates configs/default.ini from the frontend's built-in defaults, so the
# example in the repo cannot drift from what a first run writes. Run after
# touching Config::write_default(); CI diffs the result.
set -e
cd "$(dirname "$0")/.."
bin=${1:-build/host/src/frontend/sdl/dsperate}
[ -x "$bin" ] || { echo "no $bin (build first, or pass the binary path)" >&2; exit 1; }
"$bin" --write-config configs/default.ini
echo "wrote configs/default.ini"
