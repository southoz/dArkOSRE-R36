#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Per-frame screen hashes of the software renderer over a fixed scene set: the
# gate every video refactor step must pass unchanged.
#
#   DS_ROMS=../resources/ds/games DS_BIOS=../resources/ds/bios \
#     tools/video_golden.sh record DIR     # write DIR/<scene>.<mode>.h
#     tools/video_golden.sh check  DIR     # rerun, compare against DIR, exit 1 on any difference
#
# Modes: "aa" (video.aa = accurate, the default) and "noaa" (--no-aa), each
# with the JIT and with --interp when the binary has a JIT. Hashes are FNV-1a
# 64 of each screen's 0xAARRGGBB (headless --hash-frames). The CPU timing model
# is DS_TIMING (default exact here); DS_TIMING=fast needs a golden directory of its own.
# HEADLESS=path overrides the binary (default build/host); DS_RUNNER=qemu-aarch64-static
# runs a cross-built one. JOBS=N sets the parallelism (default nproc).
# The JIT runs with DS_JIT_STRICT=1 (per-instruction budget checks: the mode in
# which it interleaves the CPUs exactly as the interpreter does), so a JIT build's
# hashes must equal the host interpreter's; DS_JIT_STRICT=0 checks the default mode.
set -euo pipefail
HERE=$(cd "$(dirname "$0")/.." && pwd)
cmd=${1:-}; dir=${2:-}
[ "$cmd" = record ] || [ "$cmd" = check ] || { sed -n 3,14p "$0"; exit 2; }
[ -n "$dir" ] || { echo "need DIR"; exit 2; }
: "${DS_ROMS:?set DS_ROMS}"; : "${DS_BIOS:?set DS_BIOS}"
G=${HEADLESS:-$HERE/build/host/src/frontend/headless/dsperate-headless}
R=${DS_RUNNER:-}
JOBS=${JOBS:-$(nproc)}
export DS_JIT_STRICT=${DS_JIT_STRICT-1}
export DS_TIMING=${DS_TIMING-exact}   # the goldens are the exact model; DS_TIMING=fast checks the fast one (its own directory)
export DS_IDLE_CUT=${DS_IDLE_CUT-0}   # the goldens are the exact timing; the idle cut (on by default) is checked apart
[ "$DS_JIT_STRICT" = 0 ] && unset DS_JIT_STRICT
[ -x "$G" ] || { echo "no headless binary at $G"; exit 2; }

# name|frames|extra args|rom
SCENES=(
  "mlbis|600|--save $HERE/scenes/mlbis.sav --replay $HERE/scenes/mlbis.dsin|Mario & Luigi - Bowser's Inside Story.nds"
  "sm64|600|--replay $HERE/scenes/sm64.dsin|Super Mario 64 DS.nds"
  "etody|600|--replay $HERE/scenes/etody.dsin|Etrian Odyssey.nds"
  "dbori|600|--replay $HERE/scenes/dbori.dsin|Dragon Ball - Origins.nds"
  "meteos|600|--replay $HERE/scenes/meteos.dsin|Meteos.nds"
  "artacd|600|--replay $HERE/scenes/artacd.dsin|Art Academy.nds"
  "gsdd|2400||Golden Sun - Dark Dawn.nds"
  "st|2400||Legend of Zelda, The - Spirit Tracks.nds"
  "nsmb|1500||New Super Mario Bros..nds"
  "pw2|2400||Pokemon - White Version 2.nds"
  "mkds|1500||Mario Kart DS.nds"
  "kssu|1500||Kirby Super Star Ultra.nds"
  "cvdos|1500||Castlevania - Dawn of Sorrow.nds"
  "florist|1500||Florist Shop.nds"
  "ph|1500||Legend of Zelda, The - Phantom Hourglass.nds"
)
MODES=("aa|" "noaa|--no-aa")
banner=$($R "$G" --frames 0 /dev/null 2>&1 | head -1 || true)   # captured first: grep -q under pipefail would SIGPIPE the run
if [[ $banner == *"(jit"* ]]; then
  MODES+=("aa-interp|--interp" "noaa-interp|--no-aa --interp")
fi

mkdir -p "$dir"
out=$dir; [ "$cmd" = check ] && out=$(mktemp -d)
jobs_file=$(mktemp)
for s in "${SCENES[@]}"; do
  IFS='|' read -r name frames extra rom <<< "$s"
  [ -f "$DS_ROMS/$rom" ] || { echo "  skip $name: no $rom" >&2; continue; }
  for m in "${MODES[@]}"; do
    IFS='|' read -r mode margs <<< "$m"
    printf '%s\0' "$R $(printf %q "$G") --direct --bios9 $(printf %q "$DS_BIOS/bios9.bin") --bios7 $(printf %q "$DS_BIOS/bios7.bin") --firmware $(printf %q "$DS_BIOS/firmware.bin") --frames $frames $extra $margs --hash-frames $(printf %q "$out/$name.$mode.h") $(printf %q "$DS_ROMS/$rom") > /dev/null 2>&1 || echo 'run failed: $name.$mode' >&2" >> "$jobs_file"
  done
done
xargs -0 -P "$JOBS" -I{} bash -c {} < "$jobs_file"
rm -f "$jobs_file"

if [ "$cmd" = record ]; then
  echo "recorded $(ls "$dir"/*.h | wc -l) hash files in $dir"; exit 0
fi
bad=0
for ref in "$dir"/*.h; do
  f=$out/$(basename "$ref")
  if [ ! -f "$f" ]; then echo "MISSING $(basename "$ref")"; bad=1; continue; fi
  if ! cmp -s "$ref" "$f"; then
    first=$( (diff "$ref" "$f" || true) | grep -m1 '^<' | cut -d' ' -f2)
    n=$( (diff "$ref" "$f" || true) | grep -c '^<' || true)
    echo "DIFF $(basename "$ref"): $n frames differ, first at frame $first"; bad=1
  fi
done
[ $bad = 0 ] && echo "all $(ls "$dir"/*.h | wc -l) hash files identical" && rm -rf "$out"
[ $bad = 0 ] || echo "new hashes kept in $out"
exit $bad
