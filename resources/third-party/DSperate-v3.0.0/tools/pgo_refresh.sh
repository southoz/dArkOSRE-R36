#!/bin/bash
# Regenerate the committed PGO profile (pgo/<arch>/), or check whether it has
# drifted from the tree it will be applied to.
#
#   DS_ROMS=<rom dir> DS_BIOS=<bios dir> [DS_DSI=<dsi dir>] [DS_A30_TOOLCHAIN=<dir>] \
#     tools/pgo_refresh.sh [--check] [--no-dsi] [--arm32] [--gcc N] [--native]
#                          [build-dir] [extra cmake args]
#
# --check  compares the profile's MANIFEST against this tree: the compiler and
#          the compile flags (a mismatch voids the whole profile -- CI fails the
#          configure on it), and the source files changed since the profile was
#          made (each edited function silently loses its profile). Exit 1 if
#          the compiler/flags differ, 2 if only sources moved, 0 if current.
# default  an instrumented aarch64 cross build, trained under qemu on the
#          recorded scenes, three cart boots from frame 0 and (where the dumps are there) two DSiWare titles, writes
#          the .gcda files into pgo/aarch64/ and a MANIFEST; then a "use" build
#          from the committed profile in a *different* directory, reporting how
#          many functions failed to find their profile (should be none straight
#          after a refresh). ~20 minutes on a desktop. Commit pgo/ afterwards.
# --no-dsi trains the DS scenes only, for a profile to ship without the DSi
#          dumps to hand. The DSi scenes are also skipped on their own when
#          DS_DSI is unset or has no bios/ pair in it (it wants bios/ with
#          biosdsi9/7.bin and dsifirmware.bin, and games/ with the titles). MANIFEST records which scenes actually ran.
# --gcc N  builds a *secondary* aarch64 profile with that GCC major version
#          (aarch64-linux-gnu-g++-N, installed alongside the default one) into
#          pgo/aarch64-gcc<full version>/, which CMakeLists prefers whenever a
#          build uses that compiler. It exists so a downstream distro on
#          another GCC gets PGO instead of silently falling back to an
#          unprofiled build: the fingerprint pins the exact compiler version,
#          and dArkOS (the supported Debian-based RK3326 distro) packages
#          DSperate with GCC 12.4 while CI builds with 13.3. Refresh it in the
#          same pass as the default profile, or it decays on its own.
# --native builds for the machine it is running on instead of cross-building:
#          no toolchain file and no qemu, so the training runs at native speed.
#          Meant for running this script *inside* an aarch64 container, which
#          is how an old-sysroot profile is made -- the low glibc floor
#          that spruceOS and the A30 need comes from an old sysroot, and an old
#          sysroot means an old compiler, which means its own profile. Combine
#          with --gcc N to pick that container's compiler and key the profile
#          by it (see tools/pgo_refresh_container.sh, which wraps this).
# --arm32  builds the ARM32 (armv7l) profile into pgo/armv7l instead, with the
#          vendored A30 toolchain and qemu-arm. It has to be that toolchain and
#          not the dev box's arm-linux-gnueabihf: a .gcda file is tied to the
#          compiler's minor version, and the A30 toolchain (GCC 13.2) is what
#          builds the ARM32 release, so a profile from any other compiler would
#          be refused by the fingerprint check at configure time. Each
#          architecture keeps its own profile directory, MANIFEST and scratch
#          build dir, so the two never collide. DS_A30_TOOLCHAIN names where
#          that toolchain lives (the directory holding tc-a30.cmake).
#
# Some scenes are here for paths the replays cannot reach: `nsmb`, `gsdd` and
# `st` boot the cart from frame 0, the only training for the boot path and for
# the cold translation burst that follows it, and run long enough to reach
# their attract-mode demos (NSMB's is the hardest sustained scene in the suite
# -- ~19-21 ms a frame on an RK3566); the DSiWare pair
# trains the DSi memory map, NWRAM, NDMA and the launcher hand-off, none of
# which a DS scene executes at all.
#
# The training needs the real BIOS and firmware: without them the JIT
# translates one-instruction blocks and the profile describes another program.
set -eu
CHECK=0; DSI=1; ARCH=aarch64; GCC=''; NATIVE=''
while :; do
  case "${1:-}" in
    --check)  CHECK=1; shift ;;
    --no-dsi) DSI=0; shift ;;
    --arm32)  ARCH=armv7l; shift ;;
    --gcc)    GCC=$2; shift 2 ;;
    --native) NATIVE=1; shift ;;
    --arch)   ARCH=$2; shift 2 ;;
    *) break ;;
  esac
done
HERE=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${1:-$HERE/build/pgo-gen-$ARCH${NATIVE:+-native}${GCC:+-gcc$GCC}}; shift || true
# Per architecture: the toolchain that builds its release binaries, the qemu to
# train under, and the compiler whose version goes in the MANIFEST. CMakeLists
# derives the profile directory from CMAKE_SYSTEM_PROCESSOR, so the names here
# are the ones it will look under.
case $ARCH in
  aarch64)
    if [ "$NATIVE" = 1 ]; then
      [ "$(uname -m)" = aarch64 ] || { echo "--native needs an aarch64 host (this is $(uname -m))"; exit 1; }
      TOOLCHAIN=""; Q=""
      PGO_CXX=g++${GCC:+-$GCC}
      command -v "$PGO_CXX" > /dev/null || { echo "no $PGO_CXX on this machine"; exit 1; }
    else
      TOOLCHAIN="$HERE/cmake/aarch64-linux-gnu.cmake"
      Q="qemu-aarch64-static -L /usr/aarch64-linux-gnu"
      PGO_CXX=aarch64-linux-gnu-g++${GCC:+-$GCC}
      [ -z "$GCC" ] || command -v "$PGO_CXX" > /dev/null || {
        echo "no $PGO_CXX (apt install g++-$GCC-aarch64-linux-gnu)"; exit 1; }
    fi
    ;;
  armv7l)
    A30=${DS_A30_TOOLCHAIN:?set DS_A30_TOOLCHAIN to the A30 toolchain directory (holding tc-a30.cmake)}
    TOOLCHAIN="$A30/tc-a30.cmake"
    PGO_CXX="$A30/a30/bin/arm-a30-linux-gnueabihf-g++"
    Q="qemu-arm-static -L $A30/a30/arm-a30-linux-gnueabihf/sysroot"
    [ -x "$PGO_CXX" ] || { echo "no ARM32 toolchain at $A30 (set DS_A30_TOOLCHAIN)"; exit 1; }
    ;;
  *) echo "unknown --arch $ARCH (aarch64 | armv7l)"; exit 1 ;;
esac
# A secondary profile goes in pgo/<arch>-gcc<full version>, which is the
# directory CMakeLists prefers when a build uses that compiler; the default
# profile keeps the plain pgo/<arch>.
PROFILE="$HERE/pgo/$ARCH"
[ -z "$GCC" ] || PROFILE="$PROFILE-gcc$("$PGO_CXX" -dumpfullversion)"
CONF=(-G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DDSPERATE_TESTS=OFF -DDSPERATE_PGO_DIR="$PROFILE")
if [ -n "$TOOLCHAIN" ]; then
  CONF+=(-DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" ${GCC:+-DDS_CROSS_GCC=$GCC})
else
  CONF+=(-DCMAKE_CXX_COMPILER="$PGO_CXX" ${GCC:+-DCMAKE_C_COMPILER=gcc-$GCC})
fi
CONF+=("$@")

fingerprint() {   # of a configured build dir
  cat "$1/pgo-fingerprint"
}

if [ $CHECK = 1 ]; then
  [ -f "$PROFILE/MANIFEST" ] || { echo "no profile at $PROFILE"; exit 1; }
  rm -rf "$BUILD"
  cmake -S "$HERE" -B "$BUILD" "${CONF[@]}" -DDSPERATE_PGO=OFF > /dev/null
  now=$(fingerprint "$BUILD"); was=$(sed -n 's/^fingerprint //p' "$PROFILE/MANIFEST")
  commit=$(sed -n 's/^commit //p' "$PROFILE/MANIFEST")
  echo "profile: $(sed -n 's/^date //p' "$PROFILE/MANIFEST"), commit $commit, $(sed -n 's/^compiler //p' "$PROFILE/MANIFEST")"
  if [ "$now" != "$was" ]; then echo "DRIFT: compiler or flags differ (profile $was, tree $now) -- the profile is void"; exit 1; fi
  # The SDL frontend is never trained (a headless run does not execute one line
  # of it), so changing it cannot cost a profile: leaving it in made every
  # frontend commit report drift for ever, which is how a real warning gets
  # ignored. TRAINED is what the training actually runs.
  TRAINED=(-- src ':(exclude)src/frontend/sdl')
  changed=$(git -C "$HERE" diff --name-only "$commit" "${TRAINED[@]}" | wc -l)
  if [ "$changed" -gt 0 ]; then
    echo "source drift: $changed trained files changed since the profile ($(git -C "$HERE" diff --shortstat "$commit" "${TRAINED[@]}" | sed 's/^ //'))"
    git -C "$HERE" diff --name-only "$commit" "${TRAINED[@]}" | sed 's/^/  /'
    exit 2
  fi
  echo "current"; exit 0
fi

: "${DS_ROMS:?set DS_ROMS}"; : "${DS_BIOS:?set DS_BIOS}"
# The DSi scenes are opt-out and self-disabling: a checkout without the dumps
# still produces a valid DS-only profile rather than failing halfway through.
DS_DSI=${DS_DSI:-}
DSI_OK=0; DSI_WHY=""
if [ $DSI = 0 ]; then DSI_WHY="--no-dsi"
elif [ -z "$DS_DSI" ]; then DSI_WHY="DS_DSI not set"
elif [ ! -f "$DS_DSI/bios/biosdsi9.bin" ] || [ ! -f "$DS_DSI/bios/biosdsi7.bin" ]; then DSI_WHY="no DSi BIOS pair in $DS_DSI/bios"
elif [ ! -f "$DS_DSI/bios/dsifirmware.bin" ]; then DSI_WHY="no DSi firmware in $DS_DSI/bios"
else DSI_OK=1
fi
echo "== instrumented build for $ARCH -> $PROFILE"
mkdir -p "$PROFILE"
find "$PROFILE" -name '*.gcda' -delete
rm -rf "$BUILD" "$BUILD-use"   # scratch directories: a stale cache (flags) would fingerprint the profile wrongly
cmake -S "$HERE" -B "$BUILD" "${CONF[@]}" -DDSPERATE_PGO=generate > /dev/null
ninja -C "$BUILD" dsperate-headless > /dev/null
G="$BUILD/src/frontend/headless/dsperate-headless"
C=(--direct --bios9 "$DS_BIOS/bios9.bin" --bios7 "$DS_BIOS/bios7.bin" --firmware "$DS_BIOS/firmware.bin")
train() { echo "  train: $1"; shift; $Q "$G" "${C[@]}" "$@" > /dev/null 2>&1 || echo "  (run failed: $*)"; }
echo "== training"
train mlbis  --save "$HERE/scenes/mlbis.sav"  --replay "$HERE/scenes/mlbis.dsin"  --frames 600 "$DS_ROMS/Mario & Luigi - Bowser's Inside Story.nds" &
train sm64   --replay "$HERE/scenes/sm64.dsin"   --frames 600 "$DS_ROMS/Super Mario 64 DS.nds" &
train etody  --replay "$HERE/scenes/etody.dsin"  --frames 600 "$DS_ROMS/Etrian Odyssey.nds" &
train dbori  --replay "$HERE/scenes/dbori.dsin" --frames 600 "$DS_ROMS/Dragon Ball - Origins.nds" &
train meteos --replay "$HERE/scenes/meteos.dsin" --frames 600 "$DS_ROMS/Meteos.nds" &
# Boots, like nsmb below: both reach their attract mode from a direct boot
# with no input, which is the heavy stretch the old save-state scenes stood in for.
train gsdd   --frames 2400 "$DS_ROMS/Golden Sun - Dark Dawn.nds" &
train st     --frames 2400 "$DS_ROMS/Legend of Zelda, The - Spirit Tracks.nds" &
# From frame 0: the boot path, the cold translation burst, and then the
# attract-mode demo, which starts around frame 1200 -- hence the frame count,
# and hence this being the longest run of the batch.
train nsmb   --frames 1500 "$DS_ROMS/New Super Mario Bros..nds" &
SCENES="mlbis sm64 etody dbori meteos gsdd st nsmb"
if [ $DSI_OK = 1 ]; then
  # BIOS-only DSiWare: the launcher's hand-off is emulated over a synthetic
  # NAND, so this needs the four BIOS dumps and the DSi firmware but no NAND
  # image. --dsi-persist keeps the titles' saves out of the asset directory
  # and --dsi-offline makes sure a training run never reaches the network.
  DC=(--direct --bios9 "$DS_BIOS/bios9.bin" --bios7 "$DS_BIOS/bios7.bin"
      --bios9i "$DS_DSI/bios/biosdsi9.bin" --bios7i "$DS_DSI/bios/biosdsi7.bin"
      --firmware "$DS_DSI/bios/dsifirmware.bin"
      --dsi --dsi-hle-launch --dsi-offline --dsi-persist "$BUILD/dsi-saves")
  mkdir -p "$BUILD/dsi-saves"
  train_dsi() { echo "  train: $1"; shift; $Q "$G" "${DC[@]}" "$@" > /dev/null 2>&1 || echo "  (run failed: $*)"; }
  train_dsi dsi-shantae --frames 600 "$DS_DSI/games/Shantae - Risky's Revenge.nds" &
  train_dsi dsi-pvz     --frames 600 "$DS_DSI/games/Plants vs Zombies.nds" &
  SCENES="$SCENES dsi-shantae dsi-pvz"
else
  echo "  (DSi scenes skipped: $DSI_WHY)"
fi
wait
n=$(find "$PROFILE" -name '*.gcda' | wc -l)
echo "  profiles: $n"
[ "$n" -gt 0 ] || { echo "no profile written"; exit 1; }
{
  echo "fingerprint $(fingerprint "$BUILD")"
  echo "compiler $("$PGO_CXX" -dumpfullversion) ($("$PGO_CXX" -dumpmachine))"
  # DS_PGO_COMMIT lets a container without git record the right commit.
  echo "commit ${DS_PGO_COMMIT:-$(git -C "$HERE" rev-parse HEAD)}"
  echo "date $(date -u +%Y-%m-%dT%H:%MZ)"
  echo "scenes $SCENES"
} > "$PROFILE/MANIFEST"

echo "== verifying: optimised build from the committed profile (separate build dir)"
VBUILD="$BUILD-use"
cmake -S "$HERE" -B "$VBUILD" "${CONF[@]}" -DDSPERATE_PGO=use -DDSPERATE_PGO_STRICT=ON > /dev/null
ninja -C "$VBUILD" > "$VBUILD/pgo-warnings.log" 2>&1 || { tail -20 "$VBUILD/pgo-warnings.log"; exit 1; }
# -Wmissing-profile is per object (no .gcda at all), -Wcoverage-mismatch per
# function (the profile is there but the function changed). Whole groups are
# expected to have no profile, because a headless training run never executes
# them at all: the SDL frontend, the achievement code, the standalone tools,
# and the two fallbacks (miniz's inflate, the reference kernels). What matters
# is whether anything *outside* those groups went untrained -- that is a scene
# that stopped running, and it used to hide behind a count.
UNTRAINED='src/frontend/sdl/|rcheevos|cheevos|tools#|miniz|kernels_ref'
missing=$(grep -c "data file not found" "$VBUILD/pgo-warnings.log" || true)
unexpected=$(grep "data file not found" "$VBUILD/pgo-warnings.log" | grep -cE -v "$UNTRAINED" || true)
mismatch=$(grep -c "control flow of function" "$VBUILD/pgo-warnings.log" || true)   # "source locations ... changed" is benign: the counts still apply
echo "  objects without a profile: $missing, of which $unexpected outside the never-trained groups (should be 0)"
[ "$unexpected" = 0 ] || grep "data file not found" "$VBUILD/pgo-warnings.log" | grep -E -v "$UNTRAINED" | sed -E 's/.*dir#(.*)\.gcda.*/    \1/'
echo "  functions whose profile no longer matches: $mismatch (should be 0 straight after a refresh)"
echo "done: $PROFILE ($n files); binaries in $VBUILD"
